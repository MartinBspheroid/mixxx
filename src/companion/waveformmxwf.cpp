#include "companion/waveformmxwf.h"

#include <QDataStream>
#include <QIODevice>

#include "waveform/waveform.h"

namespace mixxx {
namespace companion {

QByteArray encodeWaveformSummaryMxwf(
        quint32 trackId, const Waveform& waveform, double durationSeconds) {
    // getDataSize() counts interleaved L/R entries (even = Left, odd = Right),
    // so the number of visual frames is half that.
    const int dataSize = waveform.getDataSize();
    const int frames = dataSize / 2;

    QByteArray blob;
    QDataStream ds(&blob, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
    ds.writeRawData("MXWF", 4);
    ds << static_cast<quint16>(1); // version
    ds << static_cast<quint16>(0); // flags (0 = mono-mixed)
    ds << trackId;
    ds << static_cast<float>(durationSeconds);
    ds << static_cast<quint32>(frames); // sampleCount (visual frames)
    ds << static_cast<quint8>(1);       // channels (mono-mixed)
    ds << static_cast<quint8>(4);       // bands: all, low, mid, high
    ds << static_cast<quint16>(0);      // reserved
    ds << static_cast<quint64>(0);      // reserved

    for (int f = 0; f < frames; ++f) {
        const int l = 2 * f;
        const int r = l + 1;
        ds << static_cast<quint8>(qMax(waveform.getAll(l), waveform.getAll(r)));
        ds << static_cast<quint8>(qMax(waveform.getLow(l), waveform.getLow(r)));
        ds << static_cast<quint8>(qMax(waveform.getMid(l), waveform.getMid(r)));
        ds << static_cast<quint8>(qMax(waveform.getHigh(l), waveform.getHigh(r)));
    }
    return blob;
}

} // namespace companion
} // namespace mixxx
