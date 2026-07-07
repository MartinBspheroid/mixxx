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
    };

    const int m_intervalMs;
    int m_numDecks;
    QTimer* m_pTimer;
    QElapsedTimer m_clock;
    QVector<DeckSample> m_samples;
};

} // namespace companion
} // namespace mixxx
