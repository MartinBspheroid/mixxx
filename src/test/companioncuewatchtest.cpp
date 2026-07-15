#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include "audio/types.h"
#include "companion/trackserializer.h"
#include "track/cue.h"
#include "track/track.h"

using mixxx::companion::serializeCues;

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);

TrackPointer makeTrack() {
    TrackPointer pTrack(Track::newTemporary());
    pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
            kSampleRate,
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(180));
    return pTrack;
}

/// Counts Track::cuesUpdated the same way CompanionService::watchTrack() hooks
/// it, so these tests pin the contract the deck.cues push actually rests on: if
/// a cue edit does not notify, the phone silently keeps stale markers.
class CueWatcher {
  public:
    explicit CueWatcher(const TrackPointer& pTrack) {
        QObject::connect(pTrack.get(), &Track::cuesUpdated, [this]() {
            m_count++;
        });
    }
    int count() const {
        return m_count;
    }
    void reset() {
        m_count = 0;
    }

  private:
    int m_count = 0;
};

QJsonArray cuesOf(const TrackPointer& pTrack) {
    return serializeCues(pTrack).value("cues").toArray();
}

TEST(CompanionCueWatchTest, AddingACueNotifiesAndAppears) {
    TrackPointer pTrack = makeTrack();
    CueWatcher watcher(pTrack);

    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(44100),
            mixxx::audio::FramePos());

    EXPECT_GT(watcher.count(), 0) << "adding a hotcue did not notify";
    const QJsonArray cues = cuesOf(pTrack);
    ASSERT_EQ(cues.size(), 1);
    EXPECT_NEAR(cues.at(0).toObject().value("positionSeconds").toDouble(),
            1.0,
            1e-9);
}

TEST(CompanionCueWatchTest, DeletingACueNotifiesAndDisappears) {
    TrackPointer pTrack = makeTrack();
    const CuePointer pCue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(44100),
            mixxx::audio::FramePos());
    CueWatcher watcher(pTrack);

    pTrack->removeCue(pCue);

    EXPECT_GT(watcher.count(), 0) << "removing a hotcue did not notify";
    EXPECT_TRUE(cuesOf(pTrack).isEmpty());
}

// Moving a cue keeps the same Cue object, so this only works because Track
// re-emits cuesUpdated from each Cue::updated -- worth pinning.
TEST(CompanionCueWatchTest, MovingACueNotifiesWithTheNewPosition) {
    TrackPointer pTrack = makeTrack();
    const CuePointer pCue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(44100), // 1.0s
            mixxx::audio::FramePos());
    CueWatcher watcher(pTrack);

    pCue->setStartPosition(mixxx::audio::FramePos(88200)); // 2.0s

    EXPECT_GT(watcher.count(), 0) << "moving a hotcue did not notify";
    const QJsonArray cues = cuesOf(pTrack);
    ASSERT_EQ(cues.size(), 1);
    EXPECT_NEAR(cues.at(0).toObject().value("positionSeconds").toDouble(),
            2.0,
            1e-9);
}

TEST(CompanionCueWatchTest, RelabelingACueNotifies) {
    TrackPointer pTrack = makeTrack();
    const CuePointer pCue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(44100),
            mixxx::audio::FramePos());
    CueWatcher watcher(pTrack);

    pCue->setLabel(QStringLiteral("drop"));

    EXPECT_GT(watcher.count(), 0) << "relabeling a hotcue did not notify";
    const QJsonArray cues = cuesOf(pTrack);
    ASSERT_EQ(cues.size(), 1);
    EXPECT_EQ(cues.at(0).toObject().value("label").toString(),
            QStringLiteral("drop"));
}

TEST(CompanionCueWatchTest, RecoloringACueNotifies) {
    TrackPointer pTrack = makeTrack();
    const CuePointer pCue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(44100),
            mixxx::audio::FramePos());
    CueWatcher watcher(pTrack);

    pCue->setColor(mixxx::RgbColor(0x3F51B5));

    EXPECT_GT(watcher.count(), 0) << "recoloring a hotcue did not notify";
    EXPECT_EQ(cuesOf(pTrack).at(0).toObject().value("color").toString(),
            QStringLiteral("#3f51b5"));
}

// Clearing several hotcues at once is a burst -- exactly what the 200ms
// coalescing in CompanionService is there to absorb. What matters is that the
// end state is correct and that it notified at all.
TEST(CompanionCueWatchTest, ClearingSeveralCuesNotifiesAndEmpties) {
    TrackPointer pTrack = makeTrack();
    for (int i = 0; i < 4; ++i) {
        pTrack->createAndAddCue(mixxx::CueType::HotCue,
                i,
                mixxx::audio::FramePos(44100 * (i + 1)),
                mixxx::audio::FramePos());
    }
    ASSERT_EQ(cuesOf(pTrack).size(), 4);
    CueWatcher watcher(pTrack);

    pTrack->removeCuesOfType(mixxx::CueType::HotCue);

    EXPECT_GT(watcher.count(), 0) << "clearing hotcues did not notify";
    EXPECT_TRUE(cuesOf(pTrack).isEmpty());
}

} // namespace
