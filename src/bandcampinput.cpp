// Pull in the minimp3 implementation exactly once in this translation unit.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_EXT_ENABLED
#include "minimp3_ex.h"

#include "bandcampinput.h"
#include "bandcampfetcher.h"
#include "replaygainparser.h"

#include <core/track.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QLoggingCategory>
#include <QWaitCondition>
#include <QThread>
#include <QTimer>
#include <QEventLoop>
#include <QSettings>

Q_LOGGING_CATEGORY(BC_INPUT, "fy.bandcamp.input")

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {

    // ── Stream URL cache ──────────────────────────────────────────────────────
    //
    // Bandcamp CDN URLs (t4.bcbits.com) carry a signed token that expires after
    // roughly one hour. We cache the last resolved URL per Bandcamp page URL so
    // that transient failures (most notably 429 rate-limits from concurrent
    // requests made by other tools) do not cause init() to return failure and
    // block fooyin's engine thread with repeated 20-second HTTP waits, which
    // would stall local file playback as well.
    //
    // TTL is 55 minutes so we never serve an expired token. On the next load
    // after expiry a fresh resolution is made as normal.
    //
    // The cache is process-global and guarded by its own mutex, independent of
    // m_mutex and state->mutex, so it is safe to access from any thread.

    struct StreamUrlCacheEntry {
        QString   streamUrl;
        QDateTime resolvedAt;
    };

    static QMutex                              s_urlCacheMutex;
    static QHash<QString, StreamUrlCacheEntry> s_urlCache;
    static constexpr qint64 kCacheTtlSeconds = 55 * 60; // 55 minutes

    void cacheStreamUrl(const QString& bandcampUrl, qint64 trackId, int trackNum, const QString& streamUrl)
    {
        if (streamUrl.isEmpty())
            return;
        const QString cacheKey = bandcampUrl + u"::"_s + QString::number(trackId) + u"::"_s + QString::number(trackNum);
        QMutexLocker cacheLock(&s_urlCacheMutex);
        s_urlCache.insert(cacheKey, {streamUrl, QDateTime::currentDateTimeUtc()});
    }

    // Shared thread-safe state to prevent Use-After-Free crashes during downloads
    struct BandcampStreamState {
        QMutex mutex;
        QWaitCondition waitCondition;
        QByteArray data;
        bool finished{false};
        bool cancelRequested{false}; // Decoupled abortion flag
    };

    // Safely block and wait for chunks to arrive over the network
    static void waitForStreamData(std::shared_ptr<BandcampStreamState>& state, size_t requiredSize, QMutexLocker<QMutex>& locker) {
        while (!state->finished && !state->cancelRequested && static_cast<size_t>(state->data.size()) < requiredSize) {
            state->waitCondition.wait(&state->mutex, 100);
        }
    }

    // ── BandcampReader ────────────────────────────────────────────────────────

    QStringList BandcampReader::extensions() const
    {
        return {QString::fromLatin1(kVirtualExt)};
    }

    bool BandcampReader::canReadCover() const    { return true; }
    bool BandcampReader::canWriteMetaData() const { return false; }

    bool BandcampReader::readTrack(const AudioSource& source, Track& track)
    {
        // Ensure the temporary stub file exists if missing
        if (!source.filepath.isEmpty() && !QFile::exists(source.filepath)) {
            QFileInfo fi{source.filepath};
            QDir{}.mkpath(fi.dir().path());
            QFile stub{source.filepath};
            (void)stub.open(QIODevice::WriteOnly);
        }

        const bool ok = !track.extraTag(u"BANDCAMP_URL"_s).isEmpty();
        if (!ok)
            qCWarning(BC_INPUT) << "BandcampReader: missing BANDCAMP_URL tag on track:" << track.title();
        return ok;
    }

    QByteArray BandcampReader::readCover(const AudioSource& /*source*/,
                                         const Track&        track,
                                         Track::Cover        cover)
    {
        if (cover != Track::Cover::Front)
            return {};

        const QStringList urls = track.extraTag(u"BANDCAMP_COVER_URL"_s);
        if (urls.isEmpty())
            return {};

        BandcampFetcher fetcher;
        const QByteArray data = fetcher.fetchBytes(urls.first());
        if (data.isEmpty())
            qCWarning(BC_INPUT) << "Failed to fetch cover art:" << fetcher.lastError();
        return data;
    }

    // ── BandcampDecoder ───────────────────────────────────────────────────────

    BandcampDecoder::BandcampDecoder()
    : m_dec(new mp3dec_ex_t{})
    {}

    BandcampDecoder::~BandcampDecoder()
    {
        stop();
        delete m_dec;
    }

    QStringList BandcampDecoder::extensions() const
    {
        return {QString::fromLatin1(kVirtualExt)};
    }

    bool BandcampDecoder::isSeekable() const { return true; }

    std::shared_ptr<BandcampStreamState> BandcampDecoder::getState() const
    {
        QMutexLocker lock(&m_statePtrMutex);
        return m_streamState;
    }

    // Static minimp3 callback: block and read from streaming buffer
    size_t BandcampDecoder::streamReadCb(void *buf, size_t size, void *user_data)
    {
        auto* self = static_cast<BandcampDecoder*>(user_data);
        auto state = self->getState();
        if (!state) return 0;

        QMutexLocker locker(&state->mutex);

        waitForStreamData(state, self->m_readPos + size, locker);

        if (self->m_readPos >= static_cast<size_t>(state->data.size())) {
            return 0; // EOF
        }

        const size_t available = static_cast<size_t>(state->data.size()) - self->m_readPos;
        const size_t toRead = std::min(size, available);

        if (toRead > 0) {
            memcpy(buf, state->data.constData() + self->m_readPos, toRead);
            self->m_readPos += toRead;
        }

        return toRead;
    }

    // Static minimp3 callback: update read cursor.
    //
    // Normal case: blocks here until the download reaches `position`, then
    // commits the seek instantly. This is the common path when seeking slightly
    // ahead of the buffer — the wait is brief and transparent to the user.
    //
    // Early-exit case: the wait loop exits before data arrives only when
    // cancelRequested or finished becomes true (i.e. stop() was called, or the
    // download ended short of the target). In that case we return -1 so minimp3
    // treats the seek as failed without touching its internal state. The caller
    // (seek()) will then restore the decoder to its previous position.
    int BandcampDecoder::streamSeekCb(uint64_t position, void *user_data)
    {
        auto* self = static_cast<BandcampDecoder*>(user_data);
        auto state = self->getState();
        if (!state) return -1;

        QMutexLocker locker(&state->mutex);

        // Block until data arrives, or until we are told to give up.
        while (!state->cancelRequested &&
               !state->finished &&
               static_cast<size_t>(state->data.size()) <= position) {
            state->waitCondition.wait(&state->mutex, 100);
        }

            // If the wait ended because of cancellation or a short download rather
            // than because data arrived, report failure without moving m_readPos.
            if (static_cast<size_t>(state->data.size()) <= position) {
                qCDebug(BC_INPUT) << "streamSeekCb: waited for position" << position
                << "but download stopped at" << state->data.size()
                << "bytes — seek cannot be satisfied";
                return -1;
            }

            self->m_readPos = position;
            return 0;
    }

    std::optional<AudioFormat> BandcampDecoder::init(const AudioSource& source,
                                                     const Track&       track,
                                                     DecoderOptions     /*opts*/)
    {
        QString bandcampUrl;
        {
            const QStringList tags = track.extraTag(u"BANDCAMP_URL"_s);
            if (!tags.isEmpty())
                bandcampUrl = tags.first();
        }
        if (bandcampUrl.isEmpty())
            bandcampUrl = source.filepath;

        if (bandcampUrl.isEmpty()) {
            qCWarning(BC_INPUT) << "Cannot determine Bandcamp URL for track:" << track.title();
            return {};
        }

        if (bandcampUrl.startsWith(u"bandcamp://"_s))
            bandcampUrl.replace(0, 11, u"https://"_s);
        if (bandcampUrl.endsWith(u".bcstream"_s))
            bandcampUrl.chop(9);

        qint64 trackId{0};
        const QStringList idTags = track.extraTag(u"BANDCAMP_TRACK_ID"_s);
        if (!idTags.isEmpty())
            trackId = idTags.first().toLongLong();
        const int trackNum = track.trackNumber().toInt();

        BandcampFetcher fetcher;
        const QString cacheKey = bandcampUrl + u"::"_s + QString::number(trackId) + u"::"_s + QString::number(trackNum);
        QString streamUrl;

        {
            QMutexLocker cacheLock(&s_urlCacheMutex);
            const auto it = s_urlCache.constFind(cacheKey);
            if (it != s_urlCache.constEnd() &&
                it->resolvedAt.secsTo(QDateTime::currentDateTimeUtc()) < kCacheTtlSeconds) {
                streamUrl = it->streamUrl;
            }
        }

        if (streamUrl.isEmpty()) {
            streamUrl = fetcher.resolveStreamUrl(bandcampUrl, trackId, trackNum);
            if (!streamUrl.isEmpty()) {
                QMutexLocker cacheLock(&s_urlCacheMutex);
                s_urlCache.insert(cacheKey, {streamUrl, QDateTime::currentDateTimeUtc()});
            } else {
                // Resolution failed — likely a 429 rate-limit from concurrent requests.
                // Try to fall back to a recently cached CDN URL for this track so that
                // a transient Bandcamp rate-limit does not stall fooyin's engine thread
                // and break unrelated local-file playback.
                QMutexLocker cacheLock(&s_urlCacheMutex);
                const auto it = s_urlCache.constFind(cacheKey);
                if (it != s_urlCache.constEnd()) {
                    qCWarning(BC_INPUT) << "resolveStreamUrl failed (" << fetcher.lastError()
                                        << ") — using cached CDN URL for" << track.title();
                    streamUrl = it->streamUrl;
                } else {
                    qCWarning(BC_INPUT) << "Could not resolve stream URL:" << fetcher.lastError();
                    return {};
                }
            }
        }

        qCDebug(BC_INPUT) << "Streaming" << track.title() << "from" << streamUrl;

        {
            // Acquire m_mutex and call stopInternal(), which closes m_dec and sets
            // cancelRequested on the old state under the lock. We deliberately do NOT
            // set cancelRequested before acquiring m_mutex (the old pattern) because
            // that early set races with ongoing mp3dec_ex operations (seek, read) that
            // hold m_mutex and call our callbacks — causing minimp3 to receive an
            // unexpected -1 from streamSeekCb mid-binary-search and corrupt its state.
            //
            // The 128KB wait in a superseded init() runs without m_mutex, so we can
            // always acquire m_mutex here immediately and unblock it via stopInternal().
            QMutexLocker locker{&m_mutex};
            stopInternal();
        }

        auto newState = std::make_shared<BandcampStreamState>();
        {
            QMutexLocker lock(&m_statePtrMutex);
            m_streamState = newState;
        }
        auto state = m_streamState; // Capture shared_ptr for lambda
        m_readPos = 0;

        // Run download in a dedicated background thread safely
        QThread* thread = QThread::create([state, streamUrl]() {
            QNetworkAccessManager nam;
            QNetworkRequest req{QUrl{streamUrl}};
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            req.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0");

            QNetworkReply* reply = nam.get(req);

            {
                QMutexLocker lock(&state->mutex);
                if (state->cancelRequested) {
                    reply->abort();
                    reply->deleteLater();
                    return;
                }
            }

            QObject::connect(reply, &QNetworkReply::readyRead, [state, reply]() {
                QMutexLocker lock(&state->mutex);
                if (state->cancelRequested) {
                    reply->abort();
                    return;
                }
                state->data.append(reply->readAll());
                state->waitCondition.wakeAll();
            });

            // Run local event loop to process background network events
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

            // Safety timer catches GUI aborts instantly even if the connection stalls
            QTimer cancelTimer;
            QObject::connect(&cancelTimer, &QTimer::timeout, [&]() {
                QMutexLocker lock(&state->mutex);
                if (state->cancelRequested) {
                    reply->abort();
                }
            });
            cancelTimer.start(100);

            loop.exec(); // Blocks until finished or safely aborted

            {
                QMutexLocker lock(&state->mutex);
                state->finished = true;
                state->waitCondition.wakeAll();
            }
            reply->deleteLater();
        });

        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();

        // Wait until minimp3 has enough data to build its frame index (~128 KB).
        // m_mutex is NOT held here — we deliberately release it so that a
        // concurrent init() (triggered by fooyin retrying a track) can acquire
        // it, call stopInternal(), and zero m_dec without deadlocking.
        {
            QMutexLocker lock(&state->mutex);
            waitForStreamData(state, 131072, lock);

            if (state->data.isEmpty()) {
                qCWarning(BC_INPUT) << "Stream finished but no data was received.";
                return {};
            }

            if (state->cancelRequested || abortToken().stop_requested()) {
                qCDebug(BC_INPUT) << "init() superseded or aborted — exiting";
                return {};
            }

            // If ID3v2 tag (e.g. embedded cover) extends beyond 128 KB, wait for
            // tag plus initial audio frame so LAME tag can be located.
            const size_t id3Size = id3v2TagSize(state->data);
            if (id3Size > 0 && id3Size + 8192 > static_cast<size_t>(state->data.size()) && id3Size + 8192 <= 1024 * 1024) {
                waitForStreamData(state, id3Size + 8192, lock);
            }

            if (state->cancelRequested || abortToken().stop_requested()) {
                qCDebug(BC_INPUT) << "init() superseded or aborted — exiting";
                return {};
            }
        }

        QMutexLocker locker{&m_mutex};

        // Re-check under m_mutex: in the narrow window between releasing
        // state->mutex above and acquiring m_mutex here, another init() could
        // have sneaked in and zeroed m_dec. If our state is no longer current,
        // m_dec belongs to the new init() and we must not touch it.
        {
            QMutexLocker stateLock(&m_statePtrMutex);
            if (m_streamState != state) {
                qCDebug(BC_INPUT) << "init() lost m_dec to a newer request — aborting";
                return {};
            }
        }

        // Parse ReplayGain tags from initial in-memory stream buffer if enabled
        QSettings settings;
        const bool useRg = settings.value(u"Bandcamp/ExtractReplayGain"_s, true).toBool();
        if (useRg) {
            QMutexLocker stateLock(&state->mutex);
            const ReplayGainData rg = parseReplayGain(state->data);
            if (rg.hasTrackGain || rg.hasAlbumGain || rg.hasTrackPeak || rg.hasAlbumPeak) {
                m_changedTrack = track;
                if (rg.hasTrackGain) m_changedTrack.setRGTrackGain(rg.trackGainDb);
                if (rg.hasAlbumGain) m_changedTrack.setRGAlbumGain(rg.albumGainDb);
                if (rg.hasTrackPeak) m_changedTrack.setRGTrackPeak(rg.trackPeak);
                if (rg.hasAlbumPeak) m_changedTrack.setRGAlbumPeak(rg.albumPeak);
                m_trackChanged = true;
                qCDebug(BC_INPUT) << "ReplayGain extracted for" << track.title()
                                  << "- Track Gain:" << rg.trackGainDb << "dB, Album Gain:" << rg.albumGainDb
                                  << "dB, Track Peak:" << rg.trackPeak << ", Album Peak:" << rg.albumPeak;
            } else {
                m_trackChanged = false;
                m_changedTrack = {};
            }
        } else {
            m_trackChanged = false;
            m_changedTrack = {};
        }

        m_io.read = streamReadCb;
        m_io.seek = streamSeekCb;
        m_io.read_data = this;
        m_io.seek_data = this;

        // MP3D_SEEK_TO_SAMPLE: mp3dec_ex_seek() takes a sample-count argument.
        //   Without this flag (i.e. MP3D_SEEK_TO_BYTE = 0), minimp3 interprets
        //   the position as a raw byte offset. We pass framesForDuration() which
        //   returns a sample count — so every seek was requesting a byte offset
        //   equal to the sample count (~10 M for a 4-min track at 44.1 kHz),
        //   massively overshooting the actual file size and causing every seek to
        //   block, fail, or desync. Additionally, the byte-seek path resets
        //   cur_sample to 0 on every call, breaking position tracking entirely.
        //
        // MP3D_DO_NOT_SCAN: skip the upfront full-file scan that would otherwise
        //   block here until the entire download completes. The seek index is
        //   built lazily on the first mp3dec_ex_seek call, scanning only up to
        //   the target position — which streamSeekCb already handles by waiting
        //   for the download to reach the needed byte offset.
        const int rc = mp3dec_ex_open_cb(m_dec, &m_io, MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN);

        if (rc != 0) {
            qCWarning(BC_INPUT) << "mp3dec_ex_open_cb failed, code:" << rc;
            *m_dec = {};
            return {};
        }

        const int sampleRate = m_dec->info.hz       > 0 ? m_dec->info.hz       : 44100;
        const int channels   = m_dec->info.channels  > 0 ? m_dec->info.channels  : 2;

        m_format.setSampleFormat(SampleFormat::S16);
        m_format.setSampleRate(sampleRate);
        m_format.setChannelCount(channels);

        m_initialized = true;
        return m_format;
    }

    void BandcampDecoder::start() { /* minimp3 is ready immediately after init */ }

    void BandcampDecoder::stop()
    {
        {
            QMutexLocker stateLock(&m_statePtrMutex);
            if (m_streamState) {
                QMutexLocker lock(&m_streamState->mutex);
                m_streamState->finished = true;
                m_streamState->cancelRequested = true;
                m_streamState->waitCondition.wakeAll();
            }
        }
        QMutexLocker locker{&m_mutex};
        stopInternal();
    }

    void BandcampDecoder::stopInternal()
    {
        if (m_initialized) {
            mp3dec_ex_close(m_dec);
            m_initialized = false;
        }

        *m_dec = {};
        m_trackChanged = false;
        m_changedTrack = {};

        std::shared_ptr<BandcampStreamState> state;
        {
            QMutexLocker lock(&m_statePtrMutex);
            state = std::move(m_streamState);
        }

        if (state) {
            QMutexLocker lock(&state->mutex);
            state->finished = true;
            state->cancelRequested = true;
            state->waitCondition.wakeAll();
        }
    }

    void BandcampDecoder::seek(uint64_t posMs)
    {
        QMutexLocker locker{&m_mutex};
        if (!m_initialized) return;

        const int ch = m_format.channelCount();
        const uint64_t targetSample =
        m_format.framesForDuration(posMs) * static_cast<uint64_t>(ch > 0 ? ch : 1);
        if (mp3dec_ex_seek(m_dec, targetSample) != 0)
            qCWarning(BC_INPUT) << "mp3dec_ex_seek failed for pos" << posMs << "ms (seek past end of file?)";
    }

    bool BandcampDecoder::needsMoreInput() const
    {
        auto state = getState();
        if (!state)
            return false;
        QMutexLocker locker(&state->mutex);
        return !state->finished && !state->cancelRequested;
    }

    AudioDecoder::ReadResult BandcampDecoder::readAudio(size_t bytes)
    {
        QMutexLocker locker{&m_mutex};

        if (!m_initialized)
            return ReadResult::endOfStream();

        const int    ch      = m_format.channelCount();
        const size_t samples = bytes / sizeof(mp3d_sample_t) / static_cast<size_t>(ch > 0 ? ch : 1);
        if (samples == 0)
            return ReadResult::endOfStream();

        AudioBuffer buf{m_format, currentMs()};
        buf.resize(static_cast<int>(samples) * ch * static_cast<int>(sizeof(mp3d_sample_t)));

        const size_t read = mp3dec_ex_read(
            m_dec,
            reinterpret_cast<mp3d_sample_t*>(buf.data()),
            samples * static_cast<size_t>(ch));

        if (read == 0) {
            auto state = getState();
            bool stillStreaming = false;
            if (state) {
                QMutexLocker stateLock(&state->mutex);
                stillStreaming = !state->finished && !state->cancelRequested;
            }

            if (stillStreaming) {
                return ReadResult::needMoreInput();
            }

            // Natural EOF: minimp3 has drained every frame.
            mp3dec_ex_close(m_dec);
            *m_dec = {};
            m_initialized = false;
            return ReadResult::endOfStream();
        }

        if (read < samples * static_cast<size_t>(ch))
            buf.resize(static_cast<int>(read) * static_cast<int>(sizeof(mp3d_sample_t)));

        return ReadResult::data(std::move(buf));
    }

    AudioBuffer BandcampDecoder::readBuffer(size_t bytes)
    {
        auto res = readAudio(bytes);
        return res.status == ReadStatus::DecodedAudio ? std::move(res.buffer) : AudioBuffer{};
    }

    bool BandcampDecoder::trackHasChanged() const
    {
        QMutexLocker locker{&m_mutex};
        return m_trackChanged;
    }

    Track BandcampDecoder::changedTrack() const
    {
        QMutexLocker locker{&m_mutex};
        return m_changedTrack;
    }

    uint64_t BandcampDecoder::currentMs() const
    {
        if (!m_initialized || m_format.sampleRate() == 0)
            return 0;
        // cur_sample is minimp3's running count of total interleaved samples
        // (frames × channels). To convert to milliseconds:
        //   ms = cur_sample / channels / sampleRate * 1000
        // Omitting the channel division causes the position to be reported at
        // (channels)× the real speed — for stereo the seekbar races ahead at
        // double rate, reaching the track end when only half the audio has played.
        const int ch = m_format.channelCount();
        return m_dec->cur_sample * 1000
        / (static_cast<uint64_t>(m_format.sampleRate()) * static_cast<uint64_t>(ch > 0 ? ch : 1));
    }

} // namespace Fooyin::Bandcamp
