#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include "preferences/usersettings.h"
#include "track/track_decl.h"

class Library;
class PlayerManager;
class QAbstractItemModel;
class TrackCollectionManager;
class TrackId;
class TrackModel;
class QThread;
class QTimer;

namespace mixxx {
namespace companion {

class CompanionServer;

/// Facade and owner of the Companion API. Lives on the main thread; spawns a
/// worker thread that runs the CompanionServer (sockets + publisher). Observes
/// BaseTrackPlayer load/unload signals on the main thread, serializes track
/// metadata, and forwards deck events to the server via queued connections.
///
/// Constructed unconditionally by CoreServices; start() is a no-op (zero cost)
/// unless the `[CompanionAPI] enabled` setting is true.
class CompanionService : public QObject {
    Q_OBJECT
  public:
    CompanionService(UserSettingsPointer pConfig,
            PlayerManager* pPlayerManager,
            TrackCollectionManager* pTrackCollectionManager,
            Library* pLibrary,
            QString appVersion,
            QObject* parent = nullptr);
    ~CompanionService() override;

    /// Start the service if enabled in settings. Idempotent.
    void start();
    /// Stop the service and join the worker thread. Idempotent; safe if never
    /// started.
    void stop();
    /// Stop then start, so config changes (enable/port/LAN) take effect.
    void restart();

    /// The current session pairing code (empty when not running). Main-thread
    /// safe: generated here before the worker thread spins up.
    QString sessionCode() const {
        return m_sessionCode;
    }
    /// True while the service is running (listening).
    bool isRunning() const {
        return m_running;
    }
    /// Generate a fresh session code and push it to the running server.
    void regenerateSessionCode();

  signals:
    /// Emitted after start/stop/regenerate so the Preferences page can refresh.
    void stateChanged();

  public slots:
    /// Run a library search on the main thread and return SearchResult JSON.
    /// Invoked by CompanionServer via BlockingQueuedConnection. `q` uses Mixxx's
    /// native search grammar; bpmMin/bpmMax<=0 mean "no bound".
    QByteArray runLibrarySearch(const QString& q,
            int bpmMin,
            int bpmMax,
            const QString& key,
            int limit,
            int offset);
    /// Serialize a single library track to TrackDto JSON (empty if not found).
    QByteArray getTrackJson(int trackId);
    /// Serialize a track's cue points to JSON (empty if track not found).
    QByteArray getTrackCues(int trackId);
    /// Serialize a track's beat grid (beat positions in seconds) to JSON.
    QByteArray getTrackBeatgrid(int trackId);
    /// Export a track's summary waveform as an MXWF v1 blob (empty if the
    /// completed summary is unavailable). All main thread.
    QByteArray exportWaveformSummary(int trackId);
    /// Load a track's cover art as JPEG, scaled to <=512px (empty if none).
    QByteArray getTrackCover(int trackId);
    /// List user playlists (hidden system playlists excluded). Main thread.
    QByteArray getPlaylists();
    /// Tracks of one playlist, in playlist order (empty if no such playlist).
    QByteArray getPlaylistTracks(int playlistId);
    /// List crates. Main thread.
    QByteArray getCrates();
    /// Tracks of one crate, by artist/title (empty if no such crate).
    QByteArray getCrateTracks(int crateId);
    /// Tracks of the current session-history playlist with played-at times.
    QByteArray getHistoryTracks();

  signals:
    void deckLoadedEvent(int deck, quint64 generation, const QJsonObject& track);
    void deckBeatgridEvent(
            int deck, quint64 generation, const QJsonObject& beatgrid);
    void deckCuesEvent(int deck, quint64 generation, const QJsonObject& cues);
    void deckUnloadedEvent(int deck, quint64 generation);
    void numberOfDecksChangedEvent(int numDecks);
    void libraryViewEvent(const QJsonObject& event);
    void libraryCursorEvent(const QJsonObject& event);
    void libraryChangedEvent(const QJsonObject& event);

  private:
    void connectDecks(int fromIndex, int toIndex);
    void onDeckLoaded(int deckIndex, const TrackPointer& pTrack);
    void onDeckUnloaded(int deckIndex);
    /// Follow pTrack's beat grid and cues for `deck`, replacing any previous
    /// watch.
    void watchTrack(int deck, const TrackPointer& pTrack);
    /// Mark `deck`'s grid/cues dirty; the burst timer does the actual emit.
    void queueBeatgrid(int deck);
    void queueCues(int deck);
    void startDeckDetailTimer();
    void emitBeatgrid(int deck);
    void emitCues(int deck);
    void onNumberOfDecksChanged(int numDecks);
    void onLoadToDeckRequested(int deck, int trackId, bool play);
    void onAutoDjQueueRequested(int trackId);
    void onPersistTokens(const QString& tokensJson);
    void onShowTrackModel(QAbstractItemModel* pModel);
    void onTrackSelected(const TrackPointer& pTrack);
    void emitLibraryView();
    void queueLibraryChanged(const char* field, const QSet<TrackId>& trackIds);
    QJsonObject buildCursorWindow(TrackModel* pTrackModel, int cursorRow) const;

    UserSettingsPointer m_pConfig;
    PlayerManager* m_pPlayerManager;
    TrackCollectionManager* m_pTrackCollectionManager;
    Library* m_pLibrary;
    QString m_sessionCode;
    QPointer<QAbstractItemModel> m_pLibraryModel; ///< active library view model
    QList<QMetaObject::Connection> m_libraryModelConnections;
    QJsonObject m_pendingLibraryChanged; ///< coalesced library.changed payload
    QTimer* m_pLibraryChangedTimer = nullptr;
    QHash<int, TrackPointer> m_deckTracks; ///< deck (1-based) -> loaded track
    QHash<int, QList<QMetaObject::Connection>> m_trackConnections;
    QSet<int> m_pendingBeatgridDecks; ///< decks whose grid changed since the tick
    QSet<int> m_pendingCueDecks;      ///< decks whose cues changed since the tick
    QTimer* m_pDeckDetailTimer = nullptr;
    const QString m_appVersion;

    QThread* m_pThread;
    CompanionServer* m_pServer;
    QHash<int, quint64> m_generations; ///< per-deck (1-based) load counter
    int m_connectedDecks;
    bool m_running;
};

} // namespace companion
} // namespace mixxx
