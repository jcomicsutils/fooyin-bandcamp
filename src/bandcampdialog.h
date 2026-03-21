#pragma once

/*
 * BandcampStreamDialog
 * ─────────────────────
 * The user pastes a Bandcamp album or track URL, clicks Fetch, sees the
 * track list, then uses one of two action buttons:
 *
 * Add to Playlist     – append tracks to the playlist selected in the
 * combo box (populated from PlaylistHandler).
 * Add to New Playlist – create a new playlist and auto-select it in the
 * combo so subsequent "Add to Playlist" clicks keep
 * targeting it.
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

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace Fooyin {
    class PlaylistHandler;
    class PlayerController;
    class Playlist;
}

namespace Fooyin::Bandcamp {

    class BandcampStreamDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit BandcampStreamDialog(PlaylistHandler* playlists,
                                      PlayerController* player,
                                      QWidget* parent = nullptr);
        ~BandcampStreamDialog() override;

    private slots:
        void onFetchClicked();
        void onAddToPlaylistClicked();
        void onAddToNewPlaylistClicked();
        void onFetchFinished(const AlbumInfo& info);

        // Keep the combo box in sync with fooyin's playlist list.
        void refreshPlaylistCombo();

    private:
        void setFetching(bool on);
        void setActionButtonsEnabled(bool on);
        void populateTable(const AlbumInfo& info);
        void syncTableToAlbum();

        // Append tracks to pl after a one-tick defer (crash-safety, see above).
        void deferredAppend(Playlist* pl, const AlbumInfo& album);

        PlaylistHandler* m_playlists;
        PlayerController* m_player;

        QLineEdit* m_urlEdit;
        QPushButton* m_fetchBtn;
        QTableWidget* m_table;
        QLabel* m_statusLabel;
        QComboBox* m_playlistCombo;
        QPushButton* m_addToPlaylistBtn;
        QPushButton* m_addToNewPlaylistBtn;
        QProgressBar* m_progress;

        AlbumInfo m_album;
    };

} // namespace Fooyin::Bandcamp
