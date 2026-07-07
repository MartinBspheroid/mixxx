#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include "preferences/usersettings.h"
#include "track/track_decl.h"

class PlayerManager;
class TrackCollectionManager;
class QThread;

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
            QString appVersion,
            QObject* parent = nullptr);
    ~CompanionService() override;

    /// Start the service if enabled in settings. Idempotent.
    void start();
    /// Stop the service and join the worker thread. Idempotent; safe if never
    /// started.
    void stop();

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
    /// Export a track's summary waveform as an MXWF v1 blob (empty if the
    /// completed summary is unavailable). All main thread.
    QByteArray exportWaveformSummary(int trackId);

  signals:
    void deckLoadedEvent(int deck, quint64 generation, const QJsonObject& track);
    void deckUnloadedEvent(int deck, quint64 generation);
    void numberOfDecksChangedEvent(int numDecks);

  private:
    void connectDecks(int fromIndex, int toIndex);
    void onDeckLoaded(int deckIndex, const TrackPointer& pTrack);
    void onDeckUnloaded(int deckIndex);
    void onNumberOfDecksChanged(int numDecks);
    void onLoadToDeckRequested(int deck, int trackId, bool play);
    void onPersistTokens(const QString& tokensJson);

    UserSettingsPointer m_pConfig;
    PlayerManager* m_pPlayerManager;
    TrackCollectionManager* m_pTrackCollectionManager;
    const QString m_appVersion;

    QThread* m_pThread;
    CompanionServer* m_pServer;
    QHash<int, quint64> m_generations; ///< per-deck (1-based) load counter
    int m_connectedDecks;
    bool m_running;
};

} // namespace companion
} // namespace mixxx
