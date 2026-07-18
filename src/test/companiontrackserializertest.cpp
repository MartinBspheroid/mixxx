#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "audio/types.h"
#include "companion/trackserializer.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"
#include "track/trackid.h"

#ifdef __STEM__
#include <QColor>

#include "track/steminfo.h"
#endif

using mixxx::companion::serializeBeatgrid;
using mixxx::companion::serializeCues;
using mixxx::companion::serializeTrack;

namespace {

TrackPointer makeTrack() {
    TrackPointer pTrack = Track::newDummy(
            QStringLiteral("/music/song.mp3"), TrackId(QVariant(1234)));
    pTrack->setArtist(QStringLiteral("Bicep"));
    pTrack->setTitle(QStringLiteral("Glue"));
    pTrack->setAlbum(QStringLiteral("Bicep"));
    pTrack->setRating(4);
    return pTrack;
}

QJsonObject serializeToObject(const TrackPointer& pTrack, bool exposePaths) {
    return serializeTrack(pTrack, exposePaths);
}

TEST(CompanionTrackSerializerTest, MapsCoreFields) {
    const QJsonObject dto = serializeToObject(makeTrack(), /*exposePaths*/ false);
    EXPECT_EQ(dto.value("id").toInt(), 1234);
    EXPECT_EQ(dto.value("title").toString(), QStringLiteral("Glue"));
    EXPECT_EQ(dto.value("artist").toString(), QStringLiteral("Bicep"));
    EXPECT_EQ(dto.value("album").toString(), QStringLiteral("Bicep"));
    EXPECT_EQ(dto.value("rating").toInt(), 4);
    // durationSeconds is always present (0 for a track with no audio props).
    EXPECT_TRUE(dto.contains("durationSeconds"));
}

TEST(CompanionTrackSerializerTest, TitleAndArtistAlwaysPresent) {
    // Empty metadata: title/artist keys must still exist (spec: required).
    TrackPointer pTrack = Track::newDummy(
            QStringLiteral("/music/x.mp3"), TrackId(QVariant(7)));
    const QJsonObject dto = serializeToObject(pTrack, false);
    EXPECT_TRUE(dto.contains("title"));
    EXPECT_TRUE(dto.contains("artist"));
    // Optional fields are omitted when empty/unset.
    EXPECT_FALSE(dto.contains("album"));
    EXPECT_FALSE(dto.contains("genre"));
    // bpm/rating of zero are omitted.
    EXPECT_FALSE(dto.contains("bpm"));
    EXPECT_FALSE(dto.contains("rating"));
}

TEST(CompanionTrackSerializerTest, FilePathGatedByOption) {
    const TrackPointer pTrack = makeTrack();
    // Off by default: the absolute path never leaks.
    EXPECT_FALSE(serializeToObject(pTrack, /*exposePaths*/ false)
                         .contains("location"));
    // On: the location is included.
    const QJsonObject exposed = serializeToObject(pTrack, /*exposePaths*/ true);
    EXPECT_TRUE(exposed.contains("location"));
    EXPECT_FALSE(exposed.value("location").toString().isEmpty());
}

TEST(CompanionTrackSerializerTest, NullTrackYieldsEmptyObject) {
    const QJsonObject dto = serializeTrack(TrackPointer(), false);
    EXPECT_TRUE(dto.isEmpty());
}

// --- Beat grid (deck.beatgrid payload / GET /v1/tracks/:id/beatgrid) ---

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);

TrackPointer makeTrackWithGrid(double bpm, double durationSeconds) {
    TrackPointer pTrack(Track::newTemporary());
    pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
            kSampleRate,
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(durationSeconds));
    pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(bpm)));
    return pTrack;
}

TEST(CompanionTrackSerializerTest, BeatgridEmitsBeatsInSeconds) {
    // 120 BPM from frame 0 => a beat every 0.5s, starting at 0.
    const QJsonObject dto = serializeBeatgrid(makeTrackWithGrid(120.0, 10.0));
    EXPECT_DOUBLE_EQ(dto.value("bpm").toDouble(), 120.0);
    EXPECT_TRUE(dto.value("constantTempo").toBool());

    const QJsonArray beats = dto.value("beats").toArray();
    ASSERT_GE(beats.size(), 3);
    EXPECT_NEAR(beats.at(0).toDouble(), 0.0, 1e-9);
    EXPECT_NEAR(beats.at(1).toDouble(), 0.5, 1e-9);
    EXPECT_NEAR(beats.at(2).toDouble(), 1.0, 1e-9);
}

TEST(CompanionTrackSerializerTest, BeatgridWithoutGridHasEmptyBeats) {
    // `beats` is required by the spec, so it must exist even with no grid.
    TrackPointer pTrack = Track::newDummy(
            QStringLiteral("/music/x.mp3"), TrackId(QVariant(7)));
    const QJsonObject dto = serializeBeatgrid(pTrack);
    ASSERT_TRUE(dto.contains("beats"));
    EXPECT_TRUE(dto.value("beats").toArray().isEmpty());
}

TEST(CompanionTrackSerializerTest, BeatgridNullTrackYieldsEmptyBeats) {
    const QJsonObject dto = serializeBeatgrid(TrackPointer());
    ASSERT_TRUE(dto.contains("beats"));
    EXPECT_TRUE(dto.value("beats").toArray().isEmpty());
}

TEST(CompanionTrackSerializerTest, BeatgridStopsAtTrackEnd) {
    // A constant-tempo grid is unbounded (cend() is INT_MAX), so the track
    // duration is what has to stop it: 120 BPM over 10s is 21 beats (0.0..10.0),
    // not 4096 beats running half an hour past the end of the track.
    const QJsonObject dto = serializeBeatgrid(makeTrackWithGrid(120.0, 10.0));
    const QJsonArray beats = dto.value("beats").toArray();
    EXPECT_EQ(beats.size(), 21);
    EXPECT_NEAR(beats.last().toDouble(), 10.0, 1e-9);
    EXPECT_FALSE(dto.contains("truncated"));
}

TEST(CompanionTrackSerializerTest, BeatgridIsTruncatedAndFlagged) {
    // 4096 beats at 200 BPM is ~20.5 minutes, so a 1-hour track overruns the
    // cap and must say so rather than silently returning a partial grid.
    const QJsonObject dto = serializeBeatgrid(makeTrackWithGrid(200.0, 3600.0));
    EXPECT_EQ(dto.value("beats").toArray().size(),
            mixxx::companion::kMaxBeatgridBeats);
    EXPECT_TRUE(dto.value("truncated").toBool());
}

// --- Cue points (deck.cues payload / GET /v1/tracks/:id/cues) ---

TrackPointer makeTrackForCues() {
    TrackPointer pTrack(Track::newTemporary());
    pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
            kSampleRate,
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(180));
    return pTrack;
}

QJsonObject findCueByIndex(const QJsonArray& cues, int index) {
    for (const QJsonValue& value : cues) {
        const QJsonObject cue = value.toObject();
        if (cue.value("index").toInt(-1) == index) {
            return cue;
        }
    }
    return QJsonObject();
}

// The positions are the whole point: a hotcue at frame N must come out as
// N/sampleRate seconds, or the phone draws its markers in the wrong place.
TEST(CompanionTrackSerializerTest, CuePositionsAreExactSeconds) {
    TrackPointer pTrack = makeTrackForCues();
    // 44100 frames == exactly 1.0s, 66150 == 1.5s, 3528000 == 80.0s.
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(44100),
            mixxx::audio::FramePos());
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            1,
            mixxx::audio::FramePos(66150),
            mixxx::audio::FramePos());
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            2,
            mixxx::audio::FramePos(3528000),
            mixxx::audio::FramePos());

    const QJsonArray cues = serializeCues(pTrack).value("cues").toArray();
    ASSERT_EQ(cues.size(), 3);
    EXPECT_NEAR(findCueByIndex(cues, 0).value("positionSeconds").toDouble(),
            1.0,
            1e-9);
    EXPECT_NEAR(findCueByIndex(cues, 1).value("positionSeconds").toDouble(),
            1.5,
            1e-9);
    EXPECT_NEAR(findCueByIndex(cues, 2).value("positionSeconds").toDouble(),
            80.0,
            1e-9);
}

TEST(CompanionTrackSerializerTest, CueCarriesTypeIndexLabelAndColor) {
    TrackPointer pTrack = makeTrackForCues();
    const CuePointer pCue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
            3,
            mixxx::audio::FramePos(44100),
            mixxx::audio::FramePos(),
            mixxx::RgbColor(0x3F51B5));
    pCue->setLabel(QStringLiteral("drop"));

    const QJsonArray cues = serializeCues(pTrack).value("cues").toArray();
    ASSERT_EQ(cues.size(), 1);
    const QJsonObject cue = cues.at(0).toObject();
    EXPECT_EQ(cue.value("type").toString(), QStringLiteral("hotcue"));
    EXPECT_EQ(cue.value("index").toInt(), 3);
    EXPECT_EQ(cue.value("label").toString(), QStringLiteral("drop"));
    EXPECT_EQ(cue.value("color").toString(), QStringLiteral("#3f51b5"));
}

// A loop/intro/outro has an end, and the phone needs its extent, not just where
// it starts.
TEST(CompanionTrackSerializerTest, CueWithEndReportsLength) {
    TrackPointer pTrack = makeTrackForCues();
    pTrack->createAndAddCue(mixxx::CueType::Loop,
            -1,
            mixxx::audio::FramePos(44100),   // 1.0s
            mixxx::audio::FramePos(220500)); // 5.0s

    const QJsonArray cues = serializeCues(pTrack).value("cues").toArray();
    ASSERT_EQ(cues.size(), 1);
    const QJsonObject cue = cues.at(0).toObject();
    EXPECT_EQ(cue.value("type").toString(), QStringLiteral("loop"));
    EXPECT_NEAR(cue.value("positionSeconds").toDouble(), 1.0, 1e-9);
    EXPECT_NEAR(cue.value("lengthSeconds").toDouble(), 4.0, 1e-9);
    // A plain cue has no index; the key must be absent rather than -1.
    EXPECT_FALSE(cue.contains("index"));
}

TEST(CompanionTrackSerializerTest, CuesWithoutCuesIsEmptyNotMissing) {
    const QJsonObject dto = serializeCues(makeTrackForCues());
    ASSERT_TRUE(dto.contains("cues"));
    EXPECT_TRUE(dto.value("cues").toArray().isEmpty());
}

TEST(CompanionTrackSerializerTest, CuesNullTrackYieldsEmptyCues) {
    const QJsonObject dto = serializeCues(TrackPointer());
    ASSERT_TRUE(dto.contains("cues"));
    EXPECT_TRUE(dto.value("cues").toArray().isEmpty());
}

// --- Stems (deck.loaded track.stems / static per-stem metadata) ---

#ifdef __STEM__

TEST(CompanionTrackSerializerTest, StemsCarryLabelAndColorInOrder) {
    const QList<StemInfo> stems = {
            StemInfo(QStringLiteral("Drums"), QColor(0x3F, 0x51, 0xB5)),
            StemInfo(QStringLiteral("Bass"), QColor(0x00, 0xB4, 0x00)),
            StemInfo(QStringLiteral("Melody"), QColor(0xFF, 0x99, 0x00)),
            StemInfo(QStringLiteral("Vocals"), QColor(0xE0, 0x00, 0x40))};
    const QJsonArray array = mixxx::companion::serializeStems(stems);
    ASSERT_EQ(array.size(), 4);
    EXPECT_EQ(array.at(0).toObject().value("label").toString(),
            QStringLiteral("Drums"));
    EXPECT_EQ(array.at(0).toObject().value("color").toString(),
            QStringLiteral("#3f51b5"));
    // Order is the stem index the live deck.tick joins against.
    EXPECT_EQ(array.at(3).toObject().value("label").toString(),
            QStringLiteral("Vocals"));
}

TEST(CompanionTrackSerializerTest, StemWithInvalidColorOmitsColor) {
    const QList<StemInfo> stems = {StemInfo(QStringLiteral("Drums"), QColor())};
    const QJsonArray array = mixxx::companion::serializeStems(stems);
    ASSERT_EQ(array.size(), 1);
    const QJsonObject stem = array.at(0).toObject();
    EXPECT_EQ(stem.value("label").toString(), QStringLiteral("Drums"));
    EXPECT_FALSE(stem.contains("color"));
}

TEST(CompanionTrackSerializerTest, NoStemsYieldsEmptyArray) {
    EXPECT_TRUE(mixxx::companion::serializeStems(QList<StemInfo>()).isEmpty());
}

#endif // __STEM__

} // namespace
