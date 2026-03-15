// Pull in the minimp3 implementation exactly once in this translation unit.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_EXT_ENABLED
#include "minimp3_ex.h"

#include "bandcampinput.h"
#include "bandcampfetcher.h"

#include <core/track.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QLoggingCategory>
#include <QWaitCondition>
#include <QThread>
#include <QTimer>
#include <QEventLoop>

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
            state->waitCondition.wait(&state->mutex);
        }
    }

    // ── BandcampReader ────────────────────────────────────────────────────────

    QStringList BandcampReader::extensions() const
    {
        return {QString::fromLatin1(kVirtualExt)};
    }

    bool BandcampReader::canReadCover() const    { return true; }
    bool BandcampReader::canWriteMetaData() const { return false; }

    bool BandcampReader::readTrack(const AudioSource& /*source*/, Track& track)
    {
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
            state->waitCondition.wait(&state->mutex);
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

        BandcampFetcher fetcher;
        QString streamUrl = fetcher.resolveStreamUrl(bandcampUrl);

        if (streamUrl.isEmpty()) {
            // Resolution failed — likely a 429 rate-limit from concurrent requests.
            // Try to fall back to a recently cached CDN URL for this track so that
            // a transient Bandcamp rate-limit does not stall fooyin's engine thread
            // and break unrelated local-file playback.
            QMutexLocker cacheLock(&s_urlCacheMutex);
            const auto it = s_urlCache.constFind(bandcampUrl);
            if (it != s_urlCache.constEnd() &&
                it->resolvedAt.secsTo(QDateTime::currentDateTimeUtc()) < kCacheTtlSeconds) {
                qCWarning(BC_INPUT) << "resolveStreamUrl failed (" << fetcher.lastError()
                << ") — using cached CDN URL for" << track.title();
            streamUrl = it->streamUrl;
                } else {
                    qCWarning(BC_INPUT) << "Could not resolve stream URL:" << fetcher.lastError();
                    return {};
                }
        } else {
            // Cache the freshly resolved URL so future failures can fall back to it.
            QMutexLocker cacheLock(&s_urlCacheMutex);
            s_urlCache.insert(bandcampUrl, {streamUrl, QDateTime::currentDateTimeUtc()});
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
        //
        // That creates a race: if a newer init() runs while we are waiting here,
        // it will set cancelRequested on our state and zero m_dec. We must
        // therefore check cancelRequested *after* the wait, before we touch m_dec,
        // otherwise we call mp3dec_ex_open_cb on a zeroed struct → SIGSEGV.
        {
            QMutexLocker lock(&state->mutex);
            waitForStreamData(state, 131072, lock);

            if (state->data.isEmpty()) {
                qCWarning(BC_INPUT) << "Stream finished but no data was received.";
                return {};
            }

            // A newer init() superseded us while we were waiting.
            // m_dec has already been zeroed by its stopInternal() call — bail out
            // before we acquire m_mutex and try to open a destroyed decoder.
            if (state->cancelRequested) {
                qCDebug(BC_INPUT) << "init() superseded by a newer request — aborting";
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
        // Do NOT set cancelRequested here before acquiring m_mutex.
        //
        // cancelRequested is visible to streamSeekCb and streamReadCb, which are
        // called from within mp3dec_ex_seek / mp3dec_ex_read while m_mutex IS held.
        // Setting it before we hold m_mutex means a concurrent seek() or readBuffer()
        // could be mid-operation (e.g. minimp3 doing a binary search in mp3dec_ex_seek)
        // when the flag flips. streamSeekCb would then return -1 unexpectedly, leaving
        // minimp3's internal frame-index state corrupt, causing SIGSEGV on the next read.
        //
        // stopInternal() is called under m_mutex and sets cancelRequested there — that
        // is sufficient. The 128KB wait in init() runs without m_mutex, so stop() can
        // always acquire m_mutex immediately and unblock it through stopInternal().
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

        // streamSeekCb blocks until the download reaches the requested byte
        // offset, so seeks into not-yet-downloaded data simply wait transparently.
        //
        // The only case streamSeekCb returns -1 is when the download has already
        // finished and the target is beyond the file's actual end (e.g. the user
        // dragged the seekbar past what the CDN delivered). In that case minimp3
        // leaves cur_sample unchanged — no restore needed, and no corrupt state.
        //
        // We deliberately do NOT restore the previous position on failure: doing
        // so puts the audio back at the old spot while the seekbar has already
        // moved forward, desyncing them. A silent no-op on an out-of-range seek
        // is far preferable — the audio just continues from wherever it was,
        // which at least stays consistent with what minimp3 reports via cur_sample.
        //
        // framesForDuration() returns PCM frames (samples per channel). With
        // MP3D_SEEK_TO_SAMPLE, mp3dec_ex_seek() expects a position in total
        // interleaved samples (frames × channels) — the same unit that cur_sample
        // counts. Forgetting to multiply means every seek lands at (1/channels)
        // of the intended position, while the seekbar jumps to the correct spot.
        const int ch = m_format.channelCount();
        const uint64_t targetSample =
        m_format.framesForDuration(posMs) * static_cast<uint64_t>(ch > 0 ? ch : 1);
        if (mp3dec_ex_seek(m_dec, targetSample) != 0)
            qCWarning(BC_INPUT) << "mp3dec_ex_seek failed for pos" << posMs << "ms (seek past end of file?)";
    }

    AudioBuffer BandcampDecoder::readBuffer(size_t bytes)
    {
        QMutexLocker locker{&m_mutex};

        if (!m_initialized)
            return {};

        const int    ch      = m_format.channelCount();
        const size_t samples = bytes / sizeof(mp3d_sample_t) / static_cast<size_t>(ch > 0 ? ch : 1);
        if (samples == 0)
            return {};

        AudioBuffer buf{m_format, currentMs()};
        buf.resize(static_cast<int>(samples) * ch * static_cast<int>(sizeof(mp3d_sample_t)));

        const size_t read = mp3dec_ex_read(
            m_dec,
            reinterpret_cast<mp3d_sample_t*>(buf.data()),
                                           samples * static_cast<size_t>(ch));

        if (read == 0) {
            // Natural EOF: minimp3 has drained every frame. With MP3D_DO_NOT_SCAN
            // the lazily-built frame index may be in an indeterminate state once
            // the last frame is consumed, so calling mp3dec_ex_close() on it later
            // (from stopInternal() during a replay init()) can SIGSEGV. Close and
            // reset right here, while the decoder is still in a well-defined state,
            // so stopInternal() sees m_initialized = false and skips the close.
            mp3dec_ex_close(m_dec);
            *m_dec = {};
            m_initialized = false;
            return {};
        }

        if (read < samples * static_cast<size_t>(ch))
            buf.resize(static_cast<int>(read) * static_cast<int>(sizeof(mp3d_sample_t)));

        return buf;
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
