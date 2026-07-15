#include "companion/trackserializer.h"

#include <QJsonArray>

#include "track/beats.h"
#include "track/cue.h"
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

QString cueTypeString(mixxx::CueType type) {
    switch (type) {
    case mixxx::CueType::HotCue:
        return QStringLiteral("hotcue");
    case mixxx::CueType::MainCue:
        return QStringLiteral("maincue");
    case mixxx::CueType::Loop:
        return QStringLiteral("loop");
    case mixxx::CueType::Intro:
        return QStringLiteral("intro");
    case mixxx::CueType::Outro:
        return QStringLiteral("outro");
    default:
        // Invalid/Beat/Jump/N60dBSound are not exposed to companion clients.
        return QString();
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

QJsonObject serializeBeatgrid(const TrackPointer& pTrack) {
    QJsonObject dto;
    QJsonArray beats;
    if (!pTrack) {
        dto.insert(QStringLiteral("beats"), beats);
        return dto;
    }

    const double bpm = pTrack->getBpm();
    if (bpm > 0.0) {
        dto.insert(QStringLiteral("bpm"), bpm);
    }

    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (pBeats) {
        dto.insert(QStringLiteral("constantTempo"), pBeats->hasConstantTempo());
        const double sampleRate = pBeats->getSampleRate().value();
        const double durationSeconds = pTrack->getDuration();
        if (sampleRate > 0.0) {
            int count = 0;
            for (auto it = pBeats->iteratorFrom(mixxx::audio::kStartFramePos);
                    it != pBeats->cend();
                    ++it) {
                const mixxx::audio::FramePos position = *it;
                if (!position.isValid()) {
                    // Defensive: cend() is effectively unbounded, so a broken
                    // grid must not spin this loop.
                    break;
                }
                const double seconds = position.value() / sampleRate;
                if (durationSeconds > 0.0 && seconds > durationSeconds) {
                    // A constant-tempo grid extends forever; the track does not.
                    break;
                }
                if (count >= kMaxBeatgridBeats) {
                    dto.insert(QStringLiteral("truncated"), true);
                    break;
                }
                beats.append(seconds);
                count++;
            }
        }
    }
    dto.insert(QStringLiteral("beats"), beats);
    return dto;
}

QJsonObject serializeCues(const TrackPointer& pTrack) {
    QJsonObject dto;
    QJsonArray cues;
    if (!pTrack) {
        dto.insert(QStringLiteral("cues"), cues);
        return dto;
    }

    const double sampleRate = pTrack->getSampleRate().value();
    const QList<CuePointer> cuePoints = pTrack->getCuePoints();
    for (const CuePointer& pCue : cuePoints) {
        if (!pCue) {
            continue;
        }
        const QString type = cueTypeString(pCue->getType());
        if (type.isEmpty()) {
            continue;
        }
        QJsonObject cue;
        cue.insert(QStringLiteral("type"), type);
        const mixxx::audio::FramePos position = pCue->getPosition();
        if (position.isValid() && sampleRate > 0.0) {
            cue.insert(QStringLiteral("positionSeconds"),
                    position.value() / sampleRate);
            const mixxx::audio::FramePos endPosition = pCue->getEndPosition();
            if (endPosition.isValid()) {
                cue.insert(QStringLiteral("lengthSeconds"),
                        (endPosition.value() - position.value()) / sampleRate);
            }
        }
        const int hotcue = pCue->getHotCue();
        if (hotcue >= 0) {
            cue.insert(QStringLiteral("index"), hotcue);
        }
        insertIfNonEmpty(&cue, QStringLiteral("label"), pCue->getLabel());
        cue.insert(QStringLiteral("color"),
                mixxx::RgbColor::toQString(pCue->getColor()));
        cues.append(cue);
    }
    dto.insert(QStringLiteral("cues"), cues);
    return dto;
}

} // namespace companion
} // namespace mixxx
