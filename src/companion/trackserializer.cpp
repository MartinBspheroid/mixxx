#include "companion/trackserializer.h"

#include "track/track.h"
#include "util/color/rgbcolor.h"

namespace mixxx {
namespace companion {

namespace {
void insertIfNonEmpty(
        QJsonObject* pObject, const QString& key, const QString& value) {
    if (!value.isEmpty()) {
        pObject->insert(key, value);
    }
}
} // namespace

QJsonObject serializeTrack(const TrackPointer& pTrack, bool exposeFilePaths) {
    QJsonObject dto;
    if (!pTrack) {
        return dto;
    }

    const TrackId id = pTrack->getId();
    if (id.isValid()) {
        dto.insert(QStringLiteral("id"), id.toVariant().toInt());
    }

    // title/artist are always present (may be empty strings).
    dto.insert(QStringLiteral("title"), pTrack->getTitle());
    dto.insert(QStringLiteral("artist"), pTrack->getArtist());
    insertIfNonEmpty(&dto, QStringLiteral("album"), pTrack->getAlbum());
    insertIfNonEmpty(&dto, QStringLiteral("genre"), pTrack->getGenre());
    insertIfNonEmpty(&dto, QStringLiteral("comment"), pTrack->getComment());

    const double bpm = pTrack->getBpm();
    if (bpm > 0.0) {
        dto.insert(QStringLiteral("bpm"), bpm);
    }
    insertIfNonEmpty(&dto, QStringLiteral("key"), pTrack->getKeyText());

    dto.insert(QStringLiteral("durationSeconds"), pTrack->getDuration());

    const int rating = pTrack->getRating();
    if (rating > 0) {
        dto.insert(QStringLiteral("rating"), rating);
    }
    dto.insert(QStringLiteral("playCount"), pTrack->getTimesPlayed());

    const QString color = mixxx::RgbColor::toQString(pTrack->getColor());
    insertIfNonEmpty(&dto, QStringLiteral("color"), color);

    if (exposeFilePaths) {
        insertIfNonEmpty(&dto, QStringLiteral("location"), pTrack->getLocation());
    }

    return dto;
}

} // namespace companion
} // namespace mixxx
