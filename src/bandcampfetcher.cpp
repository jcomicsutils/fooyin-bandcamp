#include "bandcampfetcher.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(BC_FETCH, "fy.bandcamp.fetch")

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {

    BandcampFetcher::BandcampFetcher() = default;

    QString BandcampFetcher::lastError() const { return m_lastError; }

    // ── Public API ────────────────────────────────────────────────────────────

    AlbumInfo BandcampFetcher::fetchAlbum(const QString& url)
    {
        m_lastError.clear();
        const QByteArray html = httpGet(url, kPageTimeoutMs);
        if (html.isEmpty())
            return {};
        return parsePage(html, url);
    }

    QString BandcampFetcher::resolveStreamUrl(const QString& bandcampTrackUrl)
    {
        m_lastError.clear();
        const QByteArray html = httpGet(bandcampTrackUrl, kPageTimeoutMs);
        if (html.isEmpty())
            return {};

        const QJsonObject tralbum = extractTralbum(html);
        const QJsonArray  tracks  = tralbum.value(u"trackinfo"_s).toArray();

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

    // ── HTTP ──────────────────────────────────────────────────────────────────

    QByteArray BandcampFetcher::httpGet(const QString& url, int timeoutMs)
    {
        QNetworkAccessManager nam;
        nam.setAutoDeleteReplies(true);

        QNetworkRequest req{QUrl{url}};
        req.setHeader(QNetworkRequest::UserAgentHeader, QLatin1StringView{kUserAgent});
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

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
            m_lastError = reply->errorString();
            qCWarning(BC_FETCH) << "HTTP error for" << url << ":" << m_lastError;
            return {};
        }
        return reply->readAll();
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

    // Year from release date (Bandcamp uses several date formats)
    const QString releaseDate =
    tralbum.value(u"album_release_date"_s).toString().isEmpty()
    ? current.value(u"release_date"_s).toString()
    : tralbum.value(u"album_release_date"_s).toString();
    static const QRegularExpression yearRe(u"(\\d{4})"_s);
    const auto ym = yearRe.match(releaseDate);
    info.date = ym.hasMatch() ? ym.captured(1) : releaseDate;

    // Genres
    for (const QJsonValue& kv : tralbum.value(u"keywords"_s).toArray())
        info.genres << kv.toString();

    // Cover art — high-res _0.jpg from the #tralbumArt anchor href
    const QString htmlStr = QString::fromUtf8(html);
    static const QRegularExpression artRe(
        uR"re(id="tralbumArt"[^>]*>.*?<a[^>]+href="([^"]+)")re"_s,
        QRegularExpression::DotMatchesEverythingOption);
    if (const auto am = artRe.match(htmlStr); am.hasMatch())
        info.coverUrl = highResCoverUrl(am.captured(1));

    // Base URL (no query/fragment) for resolving relative links
    QUrl bu{pageUrl};
    bu.setQuery({});
    bu.setFragment({});
    const QString baseUrl = bu.toString();

    // Tracks
    const QJsonArray trackinfo = tralbum.value(u"trackinfo"_s).toArray();
    const int total = static_cast<int>(trackinfo.size());

    for (const QJsonValue& tv : trackinfo) {
        const QJsonObject t = tv.toObject();

        // Only include tracks with a streamable mp3-128
        if (t.value(u"file"_s).toObject().value(u"mp3-128"_s).toString().isEmpty())
            continue;

        TrackInfo ti;
        ti.title       = t.value(u"title"_s).toString();
        ti.trackNum    = t.value(u"track_num"_s).toInt();
        ti.totalTracks = total;
        ti.durationMs  = static_cast<uint64_t>(t.value(u"duration"_s).toDouble() * 1000.0);

        const QString ta = t.value(u"artist"_s).toString();
        ti.artist = ta.isEmpty() ? info.albumArtist : ta;

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
    // Convert e.g. …a1234_10.jpg  →  …a1234_0.jpg (full resolution)
    QString url = raw;
    static const QRegularExpression sizeRe(u"_(\\d+)\\.(jpg|png)$"_s);
    url.replace(sizeRe, u"_0.\\2"_s);
    return url;
}

} // namespace Fooyin::Bandcamp
