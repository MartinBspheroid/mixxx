#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>

#include "companion/trackserializer.h"
#include "track/track.h"
#include "track/trackid.h"

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

} // namespace
