#include "bandcampfetcher.h"
#include "bandcampinput.h"
#include "replaygainparser.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(BC_FETCH, "fy.bandcamp.fetch")

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {

    BandcampFetcher::BandcampFetcher() = default;

    QString BandcampFetcher::lastError() const
    {
        QMutexLocker locker(&m_errorMutex);
        return m_lastError;
    }

    // ── Public API ────────────────────────────────────────────────────────────

    AlbumInfo BandcampFetcher::fetchAlbum(const QString& url, bool extractReplayGain)
    {
        {
            QMutexLocker locker(&m_errorMutex);
            m_lastError.clear();
        }
        const QByteArray html = httpGet(url, kPageTimeoutMs);
        if (html.isEmpty())
            return {};
        AlbumInfo info = parsePage(html, url);

        // Pre-cache stream URLs so playback starts instantly without re-fetching
        for (TrackInfo& ti : info.tracks) {
            if (!ti.streamUrl.isEmpty()) {
                cacheStreamUrl(ti.bandcampUrl, ti.trackId, ti.trackNum, ti.streamUrl);
            }
        }

        if (extractReplayGain && !info.tracks.isEmpty()) {
            QThreadPool pool;
            pool.setMaxThreadCount(4);
            for (TrackInfo& ti : info.tracks) {
                if (ti.streamUrl.isEmpty())
                    continue;

                pool.start([this, &ti]() {
                    QByteArray head = fetchRange(ti.streamUrl, 0, 131071);
                    if (!head.isEmpty()) {
                        const size_t id3Size = id3v2TagSize(head);
                        if (id3Size > 0 && id3Size + 8192 > static_cast<size_t>(head.size()) && id3Size + 8192 <= 1024 * 1024) {
                            head = fetchRange(ti.streamUrl, 0, static_cast<qint64>(id3Size + 8191));
                        }
                        ti.replayGain = parseReplayGain(head);
                    }
                });
            }
            pool.waitForDone();
        }

        return info;
    }

    QString BandcampFetcher::resolveStreamUrl(const QString& bandcampTrackUrl, qint64 trackId, int trackNum)
    {
        {
            QMutexLocker locker(&m_errorMutex);
            m_lastError.clear();
        }
        const QByteArray html = httpGet(bandcampTrackUrl, kPageTimeoutMs);
        if (html.isEmpty())
            return {};

        const QJsonObject tralbum = extractTralbum(html);
        const QJsonArray  tracks  = tralbum.value(u"trackinfo"_s).toArray();

        // 1. If trackId is specified, match by track_id or id
        if (trackId > 0) {
            for (const QJsonValue& v : tracks) {
                const QJsonObject obj = v.toObject();
                const qint64 tid = obj.value(u"track_id"_s).toInteger(obj.value(u"id"_s).toInteger(0));
                if (tid == trackId) {
                    const QString mp3 = obj.value(u"file"_s).toObject().value(u"mp3-128"_s).toString();
                    if (!mp3.isEmpty())
                        return mp3;
                }
            }
        }

        // 2. If trackNum is specified, match by track_num
        if (trackNum > 0) {
            for (const QJsonValue& v : tracks) {
                const QJsonObject obj = v.toObject();
                if (obj.value(u"track_num"_s).toInt() == trackNum) {
                    const QString mp3 = obj.value(u"file"_s).toObject().value(u"mp3-128"_s).toString();
                    if (!mp3.isEmpty())
                        return mp3;
                }
            }
        }

        // 3. Fallback: return the first available mp3-128
        for (const QJsonValue& v : tracks) {
            const QString mp3 = v.toObject()
            .value(u"file"_s).toObject()
            .value(u"mp3-128"_s).toString();
            if (!mp3.isEmpty())
                return mp3;
        }

        m_lastError = u"No mp3-128 stream found for: %1"_s.arg(bandcampTrackUrl);
        qCWarning(BC_FETCH) << m_lastError;
        return {};
    }

    QByteArray BandcampFetcher::fetchBytes(const QString& url)
    {
        // MP3 files can be large (a 30-minute track at 128 kbps ≈ 28 MB).
        // Use a generous timeout so slow connections aren't cut off mid-download.
        return httpGet(url, kDownloadTimeoutMs);
    }

    QByteArray BandcampFetcher::fetchRange(const QString& url, qint64 from, qint64 to, int timeoutMs)
    {
        const QByteArray range = "bytes=" + QByteArray::number(from) + '-' + QByteArray::number(to);
        return httpGet(url, timeoutMs, range);
    }

    // ── HTTP ──────────────────────────────────────────────────────────────────

    QByteArray BandcampFetcher::httpGet(const QString& url, int timeoutMs, const QByteArray& rangeHeader)
    {
        QByteArray result;
        QString errorString;

        QMutex mutex;
        QWaitCondition cond;
        bool finished = false;

        // Run the HTTP fetch in a throwaway background thread.
        // This prevents QEventLoop from processing engine-thread events (like
        // decoder destruction/track changes) while we wait for the network.
        QThread* thread = QThread::create([&]() {
            QNetworkAccessManager nam;
            QNetworkRequest req{QUrl{url}};
            req.setHeader(QNetworkRequest::UserAgentHeader, QLatin1StringView{kUserAgent});
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
            if (!rangeHeader.isEmpty()) {
                req.setRawHeader("Range", rangeHeader);
            }

            QNetworkReply* reply = nam.get(req);

            QEventLoop loop;
            QTimer     timer;
            timer.setSingleShot(true);
            timer.setInterval(timeoutMs);
            
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
                reply->abort();
                loop.quit();
            });
            
            timer.start();
            loop.exec();

            if (reply->error() != QNetworkReply::NoError) {
                errorString = reply->errorString();
            } else {
                result = reply->readAll();
            }
            
            reply->deleteLater();

            QMutexLocker lock(&mutex);
            finished = true;
            cond.wakeAll();
        });

        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();

        // Safely block the calling thread (Engine thread) without spinning its event loop
        {
            QMutexLocker lock(&mutex);
            while (!finished) {
                cond.wait(&mutex);
            }
        }

        if (!errorString.isEmpty()) {
            QMutexLocker locker(&m_errorMutex);
            m_lastError = errorString;
            qCWarning(BC_FETCH) << "HTTP error for" << url << ":" << m_lastError;
            return {};
        }
        
        return result;
    }

    // ── Page parsing ──────────────────────────────────────────────────────────

    /* static */
    QJsonObject BandcampFetcher::extractTralbum(const QByteArray& html)
    {
        // The page embeds JSON in a script tag:  data-tralbum="{ ... }"
        // The attribute value is HTML-entity-encoded.
        static const QRegularExpression re(
            uR"re(data-tralbum="([^"]*)")re"_s,
        QRegularExpression::DotMatchesEverythingOption);

        const QString s = QString::fromUtf8(html);
        const auto    m = re.match(s);
        if (!m.hasMatch())
            return {};

        return QJsonDocument::fromJson(unescapeHtml(m.captured(1)).toUtf8()).object();
    }

    /* static */
    AlbumInfo BandcampFetcher::parsePage(const QByteArray& html, const QString& pageUrl)
    {
        AlbumInfo info;
        info.albumUrl = pageUrl;

        const QJsonObject tralbum = extractTralbum(html);
        if (tralbum.isEmpty()) {
            qCWarning(BC_FETCH) << "No data-tralbum on page:" << pageUrl;
            return info;
        }

        const QJsonObject current = tralbum.value(u"current"_s).toObject();
        info.albumArtist = tralbum.value(u"artist"_s).toString();
        info.albumTitle  = current.value(u"title"_s).toString();

        const QString releaseDate =
        tralbum.value(u"album_release_date"_s).toString().isEmpty()
        ? current.value(u"release_date"_s).toString()
        : tralbum.value(u"album_release_date"_s).toString();
        static const QRegularExpression yearRe(u"(\\d{4})"_s);
        const auto ym = yearRe.match(releaseDate);
        info.date = ym.hasMatch() ? ym.captured(1) : releaseDate;

        for (const QJsonValue& kv : tralbum.value(u"keywords"_s).toArray())
            info.genres << kv.toString();

        const QString htmlStr = QString::fromUtf8(html);
        static const QRegularExpression artRe(
            uR"re(id="tralbumArt"[^>]*>.*?<a[^>]+href="([^"]+)")re"_s,
            QRegularExpression::DotMatchesEverythingOption);
        if (const auto am = artRe.match(htmlStr); am.hasMatch())
            info.coverUrl = highResCoverUrl(am.captured(1));

        QUrl bu{pageUrl};
        bu.setQuery({});
        bu.setFragment({});
        const QString baseUrl = bu.toString();

        const QJsonArray trackinfo = tralbum.value(u"trackinfo"_s).toArray();
        const int total = static_cast<int>(trackinfo.size());

        for (const QJsonValue& tv : trackinfo) {
            const QJsonObject t = tv.toObject();

            QString streamUrl = t.value(u"file"_s).toObject().value(u"mp3-128"_s).toString();
            if (streamUrl.isEmpty())
                continue;
            if (streamUrl.startsWith(u"//"_s))
                streamUrl.prepend(u"https:"_s);

            TrackInfo ti;
            ti.streamUrl   = streamUrl;
            ti.trackId     = t.value(u"track_id"_s).toInteger(t.value(u"id"_s).toInteger(0));
            ti.title       = t.value(u"title"_s).toString();
            ti.trackNum    = t.value(u"track_num"_s).toInt();
            ti.totalTracks = total;
            ti.durationMs  = static_cast<uint64_t>(t.value(u"duration"_s).toDouble() * 1000.0);

            const QString ta = t.value(u"artist"_s).toString();
            ti.artist = ta.isEmpty() ? info.albumArtist : ta;
            
            ti.album = info.albumTitle;

            const QString link = t.value(u"title_link"_s).toString();
            ti.bandcampUrl = link.isEmpty() ? pageUrl : resolveUrl(baseUrl, link);

            info.tracks.append(ti);
        }

        return info;
    }

    /* static */
    QString BandcampFetcher::unescapeHtml(const QString& s)
    {
        QString r = s;
        r.replace(u"&amp;"_s,  u"&"_s);
        r.replace(u"&lt;"_s,   u"<"_s);
        r.replace(u"&gt;"_s,   u">"_s);
        r.replace(u"&quot;"_s, u"\""_s);
        r.replace(u"&#39;"_s,  u"'"_s);
        r.replace(u"&#x27;"_s, u"'"_s);
        static const QRegularExpression numEnt(u"&#(\\d+);"_s);
        QRegularExpressionMatchIterator it = numEnt.globalMatch(r);
        while (it.hasNext()) {
            const auto m = it.next();
            r.replace(m.captured(0), QChar(m.captured(1).toInt()));
        }
        return r;
    }

    /* static */
    QString BandcampFetcher::resolveUrl(const QString& base, const QString& rel)
    {
        return QUrl{base}.resolved(QUrl{rel}).toString();
    }

    /* static */
    QString BandcampFetcher::highResCoverUrl(const QString& raw)
    {
        QString url = raw;
        static const QRegularExpression sizeRe(u"_(\\d+)\\.(jpg|png)$"_s);
        url.replace(sizeRe, u"_0.\\2"_s);
        return url;
    }

} // namespace Fooyin::Bandcamp
