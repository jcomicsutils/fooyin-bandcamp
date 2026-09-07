#include "bandcampdialog.h"

#include "bandcampinput.h"   // kVirtualExt

#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <core/track.h>

// UId must be registered with Qt's metatype system to be stored in QVariant.
// fooyin may already declare this; if you get a compile error about UId and
// QVariant, add:  Q_DECLARE_METATYPE(Fooyin::UId)  to bandcampdialog.h.
#include <utils/id.h>

#include <QCheckBox>
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
#include <QSettings>
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

        const QString trackKey = ti.bandcampUrl + u"#"_s + QString::number(ti.trackId ? ti.trackId : ti.trackNum);
        const QString hash =
            QString::number(qHash(trackKey), 16).left(16).rightJustified(16, u'0');
        const QString stubPath = stubDir + u'/' + hash + u".bcstream"_s;

        if (!QFile::exists(stubPath)) {
            QFile stub{stubPath};
            (void)stub.open(QIODevice::WriteOnly);
        }

        Track t{stubPath};
        t.addExtraTag(u"BANDCAMP_URL"_s, bcUri);
        if (ti.trackId > 0)
            t.addExtraTag(u"BANDCAMP_TRACK_ID"_s, QString::number(ti.trackId));

        // Mark metadata as already read so fooyin's AudioLoader does not attempt
        // to re-read it from the empty stub file.  Track::metadataWasRead() is a
        // plain boolean flag; only setMetadataWasRead(true) sets it — setting
        // fileSize/modifiedTime has no effect on this flag.
        t.setMetadataWasRead(true);
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

        if (ti.replayGain.hasTrackGain) {
            t.setRGTrackGain(static_cast<float>(ti.replayGain.trackGainDb));
            t.addExtraTag(u"REPLAYGAIN_TRACK_GAIN"_s, QString::asprintf("%+.2f dB", ti.replayGain.trackGainDb));
        }
        if (ti.replayGain.hasAlbumGain) {
            t.setRGAlbumGain(static_cast<float>(ti.replayGain.albumGainDb));
            t.addExtraTag(u"REPLAYGAIN_ALBUM_GAIN"_s, QString::asprintf("%+.2f dB", ti.replayGain.albumGainDb));
        }
        if (ti.replayGain.hasTrackPeak) {
            t.setRGTrackPeak(static_cast<float>(ti.replayGain.trackPeak));
            t.addExtraTag(u"REPLAYGAIN_TRACK_PEAK"_s, QString::number(ti.replayGain.trackPeak, 'f', 6));
        }
        if (ti.replayGain.hasAlbumPeak) {
            t.setRGAlbumPeak(static_cast<float>(ti.replayGain.albumPeak));
            t.addExtraTag(u"REPLAYGAIN_ALBUM_PEAK"_s, QString::number(ti.replayGain.albumPeak, 'f', 6));
        }

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

        // ── Options Row ───────────────────────────────────────────────────────
        m_replayGainCheck = new QCheckBox(tr("Extract ReplayGain metadata"));
        QSettings settings;
        m_replayGainCheck->setChecked(settings.value(u"Bandcamp/ExtractReplayGain"_s, true).toBool());
        connect(m_replayGainCheck, &QCheckBox::toggled, [](bool checked) {
            QSettings s;
            s.setValue(u"Bandcamp/ExtractReplayGain"_s, checked);
        });

        m_cleanTempBtn = new QPushButton(tr("Clean Temp Files"));
        m_cleanTempBtn->setToolTip(tr("Delete cached stream files and stub files in /tmp/fooyin-bandcamp"));
        connect(m_cleanTempBtn, &QPushButton::clicked, this, &BandcampStreamDialog::onCleanTempClicked);

        auto* optionsRow = new QHBoxLayout;
        optionsRow->addWidget(m_replayGainCheck);
        optionsRow->addStretch();
        optionsRow->addWidget(m_cleanTempBtn);

        // ── Progress ──────────────────────────────────────────────────────────
        m_progress = new QProgressBar;
        m_progress->setRange(0, 0);
        m_progress->setVisible(false);
        m_progress->setMaximumHeight(4);

        // ── Track table ───────────────────────────────────────────────────────
        m_table = new QTableWidget(0, 6);
        m_table->setHorizontalHeaderLabels({tr("#"), tr("Title"), tr("Artist"), tr("Album"), tr("Duration"), tr("ReplayGain")});
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
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
        layout->addLayout(optionsRow);
        layout->addWidget(m_progress);
        layout->addWidget(m_table, 1);
        layout->addLayout(statusRow);
        layout->addLayout(bottomRow);

        // Populate the combo with whatever playlists already exist.
        refreshPlaylistCombo();

        // Keep the combo in sync as playlists are added/removed/renamed.
        // These signals carry a Playlist* argument; use a lambda to drop it so
        // the no-arg refreshPlaylistCombo() slot compiles.
        connect(m_playlists, &PlaylistHandler::playlistAdded,
                this, [this](Fooyin::Playlist*){ refreshPlaylistCombo(); });
        connect(m_playlists, &PlaylistHandler::playlistRemoved,
                this, [this](Fooyin::Playlist*){ refreshPlaylistCombo(); });
        connect(m_playlists, &PlaylistHandler::playlistRenamed,
                this, [this](Fooyin::Playlist*){ refreshPlaylistCombo(); });

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

        const bool extractRg = m_replayGainCheck->isChecked();
        auto* watcher = new QFutureWatcher<AlbumInfo>(this);
        connect(watcher, &QFutureWatcher<AlbumInfo>::finished, this, [this, watcher]() {
            onFetchFinished(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([url, extractRg]() {
            BandcampFetcher f;
            return f.fetchAlbum(url, extractRg);
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
        m_replayGainCheck->setEnabled(!on);
        m_cleanTempBtn->setEnabled(!on);
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

            QString rgStr = u"-"_s;
            if (ti.replayGain.hasTrackGain) {
                rgStr = QString::asprintf("%+.2f dB", ti.replayGain.trackGainDb);
            }
            auto* rgItem = new QTableWidgetItem(rgStr);

            numItem->setTextAlignment(Qt::AlignCenter);
            durItem->setTextAlignment(Qt::AlignCenter);
            rgItem->setTextAlignment(Qt::AlignCenter);

            numItem->setFlags(numItem->flags() & ~Qt::ItemIsEditable);
            durItem->setFlags(durItem->flags() & ~Qt::ItemIsEditable);
            rgItem->setFlags(rgItem->flags() & ~Qt::ItemIsEditable);

            m_table->setItem(i, 0, numItem);
            m_table->setItem(i, 1, titleItem);
            m_table->setItem(i, 2, artItem);
            m_table->setItem(i, 3, albumItem);
            m_table->setItem(i, 4, durItem);
            m_table->setItem(i, 5, rgItem);
        }
    }

    void BandcampStreamDialog::onCleanTempClicked()
    {
        const QString stubDir = QDir::tempPath() + u"/fooyin-bandcamp"_s;
        QDir dir{stubDir};
        if (!dir.exists()) {
            m_statusLabel->setText(tr("No temporary files found to clean."));
            return;
        }

        const QStringList files = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
        const int count = files.size();
        if (dir.removeRecursively()) {
            dir.mkpath(stubDir);
            m_statusLabel->setText(tr("Cleaned %1 temporary file(s).").arg(count));
        } else {
            m_statusLabel->setText(tr("Failed to clean temporary files."));
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
