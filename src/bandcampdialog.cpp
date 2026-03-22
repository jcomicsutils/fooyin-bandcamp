#include "bandcampdialog.h"

#include "bandcampinput.h"   // kVirtualExt

#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <core/track.h>

// UId must be registered with Qt's metatype system to be stored in QVariant.
// fooyin may already declare this; if you get a compile error about UId and
// QVariant, add:  Q_DECLARE_METATYPE(Fooyin::UId)  to bandcampdialog.h.
#include <utils/id.h>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(BC_DLG, "fy.bandcamp.dialog")

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {

    // ── File-local helpers ────────────────────────────────────────────────────

    static Track buildTrack(const AlbumInfo& album, const TrackInfo& ti)
    {
        // fooyin's engine calls QFile::open(track.filepath()) before our decoder's
        // init() is reached.  A custom URI like "bandcamp://" is not a real path,
        // so we write a zero-byte stub file with the ".bcstream" extension and store
        // the real Bandcamp page URL in the BANDCAMP_URL extra tag instead.
        //
        // Stub files live in <temp>/fooyin-bandcamp/ keyed by a URL hash so we
        // don't accumulate duplicates across sessions.

        QString bcUri = ti.bandcampUrl;
        bcUri.replace(u"https://"_s, u"bandcamp://"_s);
        bcUri.replace(u"http://"_s,  u"bandcamp://"_s);
        if (!bcUri.endsWith(u".bcstream"_s))
            bcUri += u".bcstream"_s;

        const QString stubDir = QDir::tempPath() + u"/fooyin-bandcamp"_s;
        QDir{}.mkpath(stubDir);

        const QString hash =
        QString::number(qHash(ti.bandcampUrl), 16).left(16).rightJustified(16, u'0');
        const QString stubPath = stubDir + u'/' + hash + u".bcstream"_s;

        if (!QFile::exists(stubPath)) {
            QFile stub{stubPath};
            (void)stub.open(QIODevice::WriteOnly);
        }

        Track t{stubPath};
        t.addExtraTag(u"BANDCAMP_URL"_s, bcUri);

        // fileSize > 0 && modifiedTime > 0 makes Track::metadataWasRead() return
        // true, so fooyin's AudioLoader skips re-reading metadata (which would
        // blank out our custom tags by reading the empty stub file).
        t.setFileSize(1);
        t.setModifiedTime(static_cast<uint64_t>(QDateTime::currentSecsSinceEpoch()));
        t.setTitle(ti.title);
        t.setArtists({ti.artist});
        t.setAlbum(ti.album);
        t.setAlbumArtists({album.albumArtist});
        t.setDate(album.date);
        t.setDuration(ti.durationMs);
        t.setGenres(album.genres);
        t.setSampleRate(44100);
        t.setChannels(2);
        t.setBitrate(128);

        if (ti.trackNum > 0)
            t.setTrackNumber(QString::number(ti.trackNum));
        if (ti.totalTracks > 0)
            t.setTrackTotal(QString::number(ti.totalTracks));

        if (!album.coverUrl.isEmpty())
            t.addExtraTag(u"BANDCAMP_COVER_URL"_s, album.coverUrl);

        return t;
    }

    static TrackList buildTracks(const AlbumInfo& album)
    {
        TrackList tracks;
        tracks.reserve(static_cast<size_t>(album.tracks.size()));
        for (const TrackInfo& ti : album.tracks)
            tracks.push_back(buildTrack(album, ti));
        return tracks;
    }

    // ── Constructor ───────────────────────────────────────────────────────────

    BandcampStreamDialog::BandcampStreamDialog(PlaylistHandler* playlists,
                                               PlayerController* player,
                                               QWidget* parent)
    : QDialog{parent}
    , m_playlists{playlists}
    , m_player{player}
    {
        setWindowTitle(tr("Stream from Bandcamp"));
        setMinimumSize(680, 440);

        // ── URL row ───────────────────────────────────────────────────────────
        m_urlEdit  = new QLineEdit;
        m_urlEdit->setPlaceholderText(tr("Paste a Bandcamp album or track URL…"));
        m_fetchBtn = new QPushButton(tr("Fetch"));

        auto* urlRow = new QHBoxLayout;
        urlRow->addWidget(m_urlEdit, 1);
        urlRow->addWidget(m_fetchBtn);

        // ── Global Album Edit Row ─────────────────────────────────────────────
        m_albumEdit = new QLineEdit;
        m_albumEdit->setPlaceholderText(tr("Global Album Name"));
        m_albumEdit->setEnabled(false);

        auto* albumRow = new QHBoxLayout;
        albumRow->addWidget(new QLabel(tr("Album:")), 0);
        albumRow->addWidget(m_albumEdit, 1);

        // ── Progress ──────────────────────────────────────────────────────────
        m_progress = new QProgressBar;
        m_progress->setRange(0, 0);
        m_progress->setVisible(false);
        m_progress->setMaximumHeight(4);

        // ── Track table ───────────────────────────────────────────────────────
        m_table = new QTableWidget(0, 5);
        m_table->setHorizontalHeaderLabels({tr("#"), tr("Title"), tr("Artist"), tr("Album"), tr("Duration")});
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setAlternatingRowColors(true);
        m_table->verticalHeader()->hide();

        // ── Bottom row ────────────────────────────────────────────────────────
        m_statusLabel         = new QLabel;
        m_playlistCombo       = new QComboBox;
        m_playlistCombo->setMinimumWidth(180);
        m_playlistCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_addToPlaylistBtn    = new QPushButton(tr("Add to Playlist"));
        m_addToNewPlaylistBtn = new QPushButton(tr("Add to New Playlist"));
        auto* closeBtn        = new QPushButton(tr("Close"));

        setActionButtonsEnabled(false);

        // Status label on its own row so it doesn't crowd the buttons.
        auto* statusRow = new QHBoxLayout;
        statusRow->addWidget(m_statusLabel, 1);

        auto* bottomRow = new QHBoxLayout;
        bottomRow->addWidget(m_playlistCombo, 1);
        bottomRow->addWidget(m_addToPlaylistBtn);
        bottomRow->addWidget(m_addToNewPlaylistBtn);
        bottomRow->addWidget(closeBtn);

        auto* layout = new QVBoxLayout(this);
        layout->addLayout(urlRow);
        layout->addLayout(albumRow);
        layout->addWidget(m_progress);
        layout->addWidget(m_table, 1);
        layout->addLayout(statusRow);
        layout->addLayout(bottomRow);

        // Populate the combo with whatever playlists already exist.
        refreshPlaylistCombo();

        // Keep the combo in sync as playlists are added/removed/renamed.
        connect(m_playlists, &PlaylistHandler::playlistAdded,
                this, &BandcampStreamDialog::refreshPlaylistCombo);
        connect(m_playlists, &PlaylistHandler::playlistRemoved,
                this, &BandcampStreamDialog::refreshPlaylistCombo);
        connect(m_playlists, &PlaylistHandler::playlistRenamed,
                this, &BandcampStreamDialog::refreshPlaylistCombo);

        connect(m_fetchBtn,            &QPushButton::clicked,     this, &BandcampStreamDialog::onFetchClicked);
        connect(m_addToPlaylistBtn,    &QPushButton::clicked,     this, &BandcampStreamDialog::onAddToPlaylistClicked);
        connect(m_addToNewPlaylistBtn, &QPushButton::clicked,     this, &BandcampStreamDialog::onAddToNewPlaylistClicked);
        connect(closeBtn,              &QPushButton::clicked,     this, &QDialog::accept);
        connect(m_urlEdit,             &QLineEdit::returnPressed, this, &BandcampStreamDialog::onFetchClicked);
        connect(m_albumEdit,           &QLineEdit::textEdited,    this, &BandcampStreamDialog::onGlobalAlbumChanged);
    }

    BandcampStreamDialog::~BandcampStreamDialog() = default;

    // ── Slots ─────────────────────────────────────────────────────────────────

    void BandcampStreamDialog::onFetchClicked()
    {
        const QString url = m_urlEdit->text().trimmed();
        if (url.isEmpty()) {
            m_statusLabel->setText(tr("Enter a Bandcamp URL first."));
            return;
        }

        setFetching(true);
        m_statusLabel->setText(tr("Fetching…"));
        m_table->setRowCount(0);
        m_albumEdit->clear();
        m_albumEdit->setEnabled(false);
        setActionButtonsEnabled(false);
        m_album = {};

        auto* watcher = new QFutureWatcher<AlbumInfo>(this);
        connect(watcher, &QFutureWatcher<AlbumInfo>::finished, this, [this, watcher]() {
            onFetchFinished(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([url]() {
            BandcampFetcher f;
            return f.fetchAlbum(url);
        }));
    }

    void BandcampStreamDialog::onFetchFinished(const AlbumInfo& info)
    {
        setFetching(false);

        if (info.tracks.isEmpty()) {
            m_statusLabel->setText(
                tr("No streamable tracks found. Make sure this is a public Bandcamp album or track URL."));
            return;
        }

        m_album = info;
        populateTable(info);
        
        m_albumEdit->setText(info.albumTitle);
        m_albumEdit->setEnabled(true);

        m_statusLabel->setText(
            tr("%1 — %2  ·  %3 track(s)")
            .arg(info.albumArtist, info.albumTitle)
            .arg(info.tracks.size()));
        setActionButtonsEnabled(true);
    }

    void BandcampStreamDialog::onGlobalAlbumChanged(const QString& text)
    {
        // Instantly sync the global album edit to all rows in the table
        for (int i = 0; i < m_table->rowCount(); ++i) {
            if (auto* item = m_table->item(i, 3)) {
                item->setText(text);
            }
        }
    }

    void BandcampStreamDialog::onAddToPlaylistClicked()
    {
        if (m_album.tracks.isEmpty() || !m_playlists)
            return;

        syncTableToAlbum();

        // The combo stores each playlist's UId as Qt::UserRole data.
        const int idx = m_playlistCombo->currentIndex();
        if (idx < 0) {
            // No playlists exist yet — create one instead.
            onAddToNewPlaylistClicked();
            return;
        }

        const auto uid = m_playlistCombo->itemData(idx).value<Fooyin::UId>();
        Playlist* pl = m_playlists->playlistById(uid);
        if (!pl) {
            // The playlist was deleted between the combo refresh and the click.
            onAddToNewPlaylistClicked();
            return;
        }

        deferredAppend(pl, m_album);
    }

    void BandcampStreamDialog::onAddToNewPlaylistClicked()
    {
        if (m_album.tracks.isEmpty() || !m_playlists)
            return;

        syncTableToAlbum();

        // Create the playlist EMPTY first, then append via a deferred call.
        //
        // createNewPlaylist(name, tracks) fires playlistAdded (model reset) and
        // track-population dataChanged in the same call stack — the model indices
        // from the reset are stale by the time dataChanged fires → SIGSEGV.
        // Separating creation from population into two event-loop ticks fixes this.
        const QString plName = u"%1 — %2"_s.arg(m_album.albumArtist, m_album.albumTitle);
        Playlist* pl = m_playlists->createNewPlaylist(plName, {});
        if (!pl) {
            m_statusLabel->setText(tr("Failed to create playlist."));
            return;
        }

        // refreshPlaylistCombo() will fire via playlistAdded signal, but that
        // happens asynchronously.  Select the new entry by UId after a tick so
        // the combo has been refreshed first.
        const auto newUid = pl->id();
        QTimer::singleShot(0, this, [this, newUid]() {
            for (int i = 0; i < m_playlistCombo->count(); ++i) {
                if (m_playlistCombo->itemData(i).value<Fooyin::UId>() == newUid) {
                    m_playlistCombo->setCurrentIndex(i);
                    break;
                }
            }
        });

        deferredAppend(pl, m_album);
    }

    void BandcampStreamDialog::refreshPlaylistCombo()
    {
        if (!m_playlists)
            return;

        // Remember what was selected so we can restore it after the repopulate.
        const auto prevUid = (m_playlistCombo->currentIndex() >= 0)
        ? m_playlistCombo->currentData().value<Fooyin::UId>()
        : Fooyin::UId{};

        m_playlistCombo->blockSignals(true);
        m_playlistCombo->clear();

        const auto pls = m_playlists->playlists();
        int restoreIdx = 0;
        int i = 0;
        for (Playlist* pl : pls) {
            m_playlistCombo->addItem(pl->name(), QVariant::fromValue(pl->id()));
            if (pl->id() == prevUid)
                restoreIdx = i;
            ++i;
        }

        if (m_playlistCombo->count() > 0)
            m_playlistCombo->setCurrentIndex(restoreIdx);

        m_playlistCombo->blockSignals(false);
    }

    // ── Private helpers ───────────────────────────────────────────────────────

    void BandcampStreamDialog::setFetching(bool on)
    {
        m_fetchBtn->setEnabled(!on);
        m_urlEdit->setEnabled(!on);
        m_progress->setVisible(on);
    }

    void BandcampStreamDialog::setActionButtonsEnabled(bool on)
    {
        m_addToPlaylistBtn->setEnabled(on);
        m_addToNewPlaylistBtn->setEnabled(on);
    }

    void BandcampStreamDialog::populateTable(const AlbumInfo& info)
    {
        m_table->setRowCount(static_cast<int>(info.tracks.size()));
        for (int i = 0; i < static_cast<int>(info.tracks.size()); ++i) {
            const TrackInfo& ti = info.tracks.at(i);

            const uint64_t sec = ti.durationMs / 1000;
            const QString  dur = u"%1:%2"_s.arg(sec / 60, 2, 10, QChar(u'0'))
            .arg(sec % 60, 2, 10, QChar(u'0'));

            auto* numItem   = new QTableWidgetItem(ti.trackNum > 0 ? QString::number(ti.trackNum) : u"-"_s);
            auto* titleItem = new QTableWidgetItem(ti.title);
            auto* artItem   = new QTableWidgetItem(ti.artist);
            auto* albumItem = new QTableWidgetItem(ti.album);
            auto* durItem   = new QTableWidgetItem(dur);

            numItem->setTextAlignment(Qt::AlignCenter);
            durItem->setTextAlignment(Qt::AlignCenter);

            numItem->setFlags(numItem->flags() & ~Qt::ItemIsEditable);
            durItem->setFlags(durItem->flags() & ~Qt::ItemIsEditable);

            m_table->setItem(i, 0, numItem);
            m_table->setItem(i, 1, titleItem);
            m_table->setItem(i, 2, artItem);
            m_table->setItem(i, 3, albumItem);
            m_table->setItem(i, 4, durItem);
        }
    }

    void BandcampStreamDialog::syncTableToAlbum()
    {
        // Update the global album title just in case you use it elsewhere
        m_album.albumTitle = m_albumEdit->text();
        
        for (int i = 0; i < m_table->rowCount() && i < m_album.tracks.size(); ++i) {
            if (auto* titleItem = m_table->item(i, 1)) {
                m_album.tracks[i].title = titleItem->text();
            }
            if (auto* artistItem = m_table->item(i, 2)) {
                m_album.tracks[i].artist = artistItem->text();
            }
            if (auto* albumItem = m_table->item(i, 3)) {
                m_album.tracks[i].album = albumItem->text();
            }
        }
    }

    void BandcampStreamDialog::deferredAppend(Playlist* pl, const AlbumInfo& album)
    {
        TrackList   tracks = buildTracks(album);
        const int   count  = static_cast<int>(tracks.size());
        const auto  uid    = pl->id();
        const QString name = pl->name();

        QTimer::singleShot(0, this, [this, uid, name, count, tracks = std::move(tracks)]() {
            if (!m_playlists)
                return;
            // Re-resolve by UId — the raw pointer may be stale if the playlist
            // was deleted between the button click and this callback.
            Playlist* target = m_playlists->playlistById(uid);
            if (!target)
                return;

            m_playlists->appendToPlaylist(uid, tracks);
            m_statusLabel->setText(
                tr("Added %1 track(s) to \"%2\"").arg(count).arg(name));
        });
    }

} // namespace Fooyin::Bandcamp
