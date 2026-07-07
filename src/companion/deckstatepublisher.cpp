#include "companion/deckstatepublisher.h"

#include <QTimer>
#include <cmath>

#include "control/controlobject.h"
#include "mixer/playermanager.h"
#include "preferences/configobject.h"

namespace {
// Change-detection thresholds. Below these deltas a deck is considered
// unchanged and no tick is sent (a keepalive still fires once per second).
constexpr double kPositionEpsilon = 1e-4;
constexpr double kVuEpsilon = 0.01;
constexpr qint64 kKeepaliveMs = 1000;

// A position jump larger than this multiple of the distance normal playback
// would cover in one interval is treated as a seek rather than a tick.
constexpr double kSeekJumpFactor = 4.0;

double controlGet(const QString& group, const char* key) {
    return ControlObject::get(ConfigKey(group, key));
}
} // namespace

namespace mixxx {
namespace companion {

DeckStatePublisher::DeckStatePublisher(int intervalMs, QObject* parent)
        : QObject(parent),
          m_intervalMs(intervalMs),
          m_numDecks(0),
          m_pTimer(new QTimer(this)) {
    m_pTimer->setInterval(m_intervalMs);
    m_pTimer->setTimerType(Qt::PreciseTimer);
    connect(m_pTimer, &QTimer::timeout, this, &DeckStatePublisher::onTimeout);
}

DeckStatePublisher::~DeckStatePublisher() = default;

void DeckStatePublisher::setDecks(int numDecks) {
    m_numDecks = qMax(0, numDecks);
    if (m_samples.size() < m_numDecks) {
        m_samples.resize(m_numDecks);
    }
}

void DeckStatePublisher::start() {
    m_clock.start();
    m_pTimer->start();
}

void DeckStatePublisher::stop() {
    m_pTimer->stop();
}

void DeckStatePublisher::onTimeout() {
    const qint64 nowMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    for (int i = 0; i < m_numDecks && i < m_samples.size(); ++i) {
        const QString group = PlayerManager::groupForDeck(i);
        const int deck = i + 1;
        DeckSample& sample = m_samples[i];

        const bool loaded = controlGet(group, "track_loaded") != 0.0;
        if (!loaded) {
            sample.loaded = false;
            continue;
        }

        const double position = controlGet(group, "playposition");
        const double duration = controlGet(group, "duration");
        const double rate = controlGet(group, "rate_ratio");
        const bool playing = controlGet(group, "play") != 0.0;
        const double vu = controlGet(group, "vu_meter");

        bool isSeek = false;
        if (sample.loaded && duration > 0.0) {
            const double expected = playing
                    ? std::fabs(rate) * (m_intervalMs / 1000.0) / duration
                    : 0.0;
            if (std::fabs(position - sample.position) >
                    expected * kSeekJumpFactor + kPositionEpsilon) {
                isSeek = true;
            }
        }

        const bool changed = !sample.loaded ||
                std::fabs(position - sample.emitPosition) > kPositionEpsilon ||
                playing != sample.playing ||
                std::fabs(vu - sample.emitVu) > kVuEpsilon;
        const bool keepalive = (nowMs - sample.emitMs) >= kKeepaliveMs;

        if (isSeek) {
            QJsonObject event;
            event.insert(QStringLiteral("type"), QStringLiteral("deck.seek"));
            event.insert(QStringLiteral("deck"), deck);
            event.insert(QStringLiteral("playposition"), position);
            emit tickReady(deck, event);
            sample.emitPosition = position;
            sample.emitVu = vu;
            sample.emitMs = nowMs;
        } else if (changed || keepalive) {
            QJsonObject event;
            event.insert(QStringLiteral("type"), QStringLiteral("deck.tick"));
            event.insert(QStringLiteral("deck"), deck);
            event.insert(QStringLiteral("playposition"), position);
            event.insert(QStringLiteral("positionSeconds"), position * duration);
            event.insert(QStringLiteral("durationSeconds"), duration);
            event.insert(QStringLiteral("rate"), rate);
            event.insert(QStringLiteral("playing"), playing);
            event.insert(QStringLiteral("vu"), vu);
            emit tickReady(deck, event);
            sample.emitPosition = position;
            sample.emitVu = vu;
            sample.emitMs = nowMs;
        }

        sample.loaded = true;
        sample.playing = playing;
        sample.position = position;
    }
}

} // namespace companion
} // namespace mixxx

#include "moc_deckstatepublisher.cpp"
