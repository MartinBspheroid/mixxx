#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QVector>

class QTimer;

namespace mixxx {
namespace companion {

/// Polls the Mixxx control system for each deck at a fixed cadence and emits
/// change-gated `deck.tick` / `deck.seek` events. Reads go through
/// `ControlObject::get()`, which is thread-safe from any thread, so the whole
/// publisher lives on the Companion worker thread with no ControlProxy
/// thread-affinity concerns.
///
/// Events are emitted without `generation` or `serverTimeMs`; CompanionServer
/// stamps those from its per-deck snapshot before broadcasting.
class DeckStatePublisher : public QObject {
    Q_OBJECT
  public:
    DeckStatePublisher(int intervalMs, QObject* parent = nullptr);
    ~DeckStatePublisher() override;

    /// Set how many decks to poll (0-based groups `[Channel1]`..`[ChannelN]`).
    void setDecks(int numDecks);

    void start();
    void stop();

  signals:
    /// A tick or seek for `deck` (1-based). `partialTick` carries `type` and the
    /// state fields; the server adds `generation` and `serverTimeMs`.
    void tickReady(int deck, const QJsonObject& partialTick);
    /// The deck configuration changed (or was first observed). `numDecks` is
    /// the engine deck count ([App],num_decks); `visibleDecks` is what the skin
    /// actually shows (2 when [Skin],show_4decks exists and is off, else
    /// numDecks). Emitted once at startup and on every change.
    void decksConfigChanged(int numDecks, int visibleDecks);
    /// Master-bus levels (droppable, change-gated): {type:"master.tick",vu,vuLeft,vuRight}.
    void masterTickReady(const QJsonObject& partialTick);

  private slots:
    void onTimeout();

  private:
    struct DeckSample {
        bool loaded = false;   ///< track_loaded seen on the previous poll
        bool playing = false;  ///< play state at last emit
        double position = 0.0; ///< playposition at previous poll (seek detection)
        double emitPosition = 0.0; ///< playposition at last emit (change gate)
        double emitVu = 0.0;   ///< vu at last emit (change gate)
        qint64 emitMs = 0;     ///< clock time of last emit (keepalive)
        bool loopEnabled = false; ///< loop state at last emit (change gate)
        int syncMode = 0;         ///< sync mode at last emit (change gate)
        bool keylock = false;     ///< keylock at last emit (change gate)
        int stemCount = 0;             ///< stem_count at last emit (0 = normal track)
        QVector<double> stemVolumes;   ///< per-stem volume at last emit (change gate)
        QVector<bool> stemMutes;       ///< per-stem mute at last emit (change gate)
    };
    double m_emitMasterVu = 0.0; ///< master vu at last emit (change gate)
    qint64 m_masterEmitMs = 0;

    const int m_intervalMs;
    int m_numDecks;
    QTimer* m_pTimer;
    QElapsedTimer m_clock;
    QVector<DeckSample> m_samples;
    int m_lastNumDecks = -1;     ///< last emitted engine deck count
    int m_lastVisibleDecks = -1; ///< last emitted visible deck count
};

} // namespace companion
} // namespace mixxx
