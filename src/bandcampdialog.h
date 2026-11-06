#pragma once

/*
 * BandcampStreamDialog
 * ─────────────────────
 * The user pastes a Bandcamp album or track URL, clicks Fetch, sees the
 * track list, then uses one of two action buttons:
 *
 * Add to Playlist        – append tracks to the playlist selected in the
 *                          combo box (or creates a new playlist if "<New Playlist>"
 *                          is selected).
 * Add to Active Playlist  – append tracks directly to fooyin's currently active playlist.
 *
 * Crash-safety note
 * ─────────────────
 * createNewPlaylist(name, tracks) fires playlistAdded (model reset) and
 * track-population dataChanged signals in the same call stack.  The model
 * indices from the reset are stale when dataChanged fires → SIGSEGV inside
 * Qt model code.  We avoid this by always creating playlists EMPTY and
 * appending tracks via QTimer::singleShot(0) (deferredAppend).
 */

#include "bandcampfetcher.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace Fooyin {
    class PlaylistHandler;
    class PlayerController;
    class CurrentPlaylistController;
    class Playlist;
}

namespace Fooyin::Bandcamp {

    class BandcampStreamDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit BandcampStreamDialog(PlaylistHandler* playlists,
                                      PlayerController* player,
                                      CurrentPlaylistController* playlistSelection = nullptr,
                                      QWidget* parent = nullptr);
        ~BandcampStreamDialog() override;

    private slots:
        void onFetchClicked();
        void onAddToPlaylistClicked();
        void onAddToActivePlaylistClicked();
        void onFetchFinished(const AlbumInfo& info);
        void onGlobalAlbumChanged(const QString& text);
        void onCleanTempClicked();

        // Keep the combo box in sync with fooyin's playlist list.
        void refreshPlaylistCombo();

    private:
        void setFetching(bool on);
        void setActionButtonsEnabled(bool on);
        void populateTable(const AlbumInfo& info);
        void syncTableToAlbum();
        void createAndAppendToNewPlaylist();
        void updateActivePlaylistButtonTooltip();

        // Append tracks to pl after a one-tick defer (crash-safety, see above).
        void deferredAppend(Playlist* pl, const AlbumInfo& album);

        PlaylistHandler*           m_playlists;
        PlayerController*          m_player;
        CurrentPlaylistController* m_playlistSelection{nullptr};

        QLineEdit* m_urlEdit;
        QPushButton* m_fetchBtn;
        QLineEdit* m_albumEdit;
        QCheckBox* m_replayGainCheck;
        QPushButton* m_cleanTempBtn;
        QTableWidget* m_table;
        QLabel* m_statusLabel;
        QComboBox* m_playlistCombo;
        QPushButton* m_addToPlaylistBtn;
        QPushButton* m_addToActivePlaylistBtn;
        QProgressBar* m_progress;

        AlbumInfo m_album;
    };

} // namespace Fooyin::Bandcamp
