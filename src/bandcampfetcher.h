#pragma once

/*
 * BandcampFetcher
 * ───────────────
 * Parses Bandcamp album/track pages and resolves CDN stream URLs.
 *
 * All network calls use a blocking QEventLoop so they can be safely called
 * from QtConcurrent worker threads.
 *
 * Track stream URLs (mp3-128 from t4.bcbits.com) expire after ~1 hour.
 * resolveStreamUrl() must therefore be called fresh at decode time, not at
 * metadata-fetch time.
 *
 * Timeouts
 * ────────
 * Page fetches (HTML, cover art): kPageTimeoutMs     =  20 s
 * MP3 downloads:                  kDownloadTimeoutMs = 300 s
 *
 * The download timeout is intentionally generous — a 30-minute track at
 * 128 kbps is ~28 MB.  On a slow connection that can take well over 20 s.
 */

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Fooyin::Bandcamp {

    struct TrackInfo {
        QString  title;
        QString  artist;        // per-track artist (VA releases), else album artist
        QString  bandcampUrl;   // https://artist.bandcamp.com/track/slug
        int      trackNum{0};
        int      totalTracks{0};
        uint64_t durationMs{0};
    };

    struct AlbumInfo {
        QString          albumTitle;
        QString          albumArtist;
        QString          albumUrl;
        QString          date;
        QString          coverUrl;   // high-res _0.jpg
        QStringList      genres;
        QList<TrackInfo> tracks;
    };

    class BandcampFetcher
    {
    public:
        BandcampFetcher();

        // Fetch and parse an album or single-track page.
        AlbumInfo fetchAlbum(const QString& url);

        // Re-resolve the time-limited mp3-128 CDN URL for a track page.
        // Call this at decode time, not at import time.
        QString resolveStreamUrl(const QString& bandcampTrackUrl);

        // Download raw bytes (cover art, MP3 data, etc.).
        QByteArray fetchBytes(const QString& url);

        QString lastError() const;

    private:
        // timeoutMs is explicit per call — use kPageTimeoutMs or kDownloadTimeoutMs.
        QByteArray httpGet(const QString& url, int timeoutMs);

        static AlbumInfo    parsePage(const QByteArray& html, const QString& pageUrl);
        static QJsonObject  extractTralbum(const QByteArray& html);
        static QString      unescapeHtml(const QString& s);
        static QString      resolveUrl(const QString& base, const QString& relative);
        static QString      highResCoverUrl(const QString& rawUrl);

        QString m_lastError;

        static constexpr const char* kUserAgent =
        "Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0";

        // 20 s for HTML pages and cover art (small payloads).
        static constexpr int kPageTimeoutMs     = 20'000;
        // 5 minutes for MP3 downloads — a 30-min track at 128 kbps is ~28 MB.
        static constexpr int kDownloadTimeoutMs = 300'000;
    };

} // namespace Fooyin::Bandcamp
