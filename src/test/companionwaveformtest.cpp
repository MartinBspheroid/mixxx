#include <gtest/gtest.h>

#include <QByteArray>
#include <QDataStream>

#include "companion/waveformmxwf.h"
#include "waveform/waveform.h"

namespace {

// assign()/resize() are private on Waveform (only the analyzer, a friend, fills
// it directly), so build a real summary-shaped Waveform via the public ctor
// (Waveform holds a QMutex and is non-copyable, so construct in place) and read
// getDataSize() rather than hardcoding it.
unsigned char patternAll(int i) {
    return static_cast<unsigned char>((i * 7) & 0xFF);
}
unsigned char patternLow(int i) {
    return static_cast<unsigned char>((i * 3 + 1) & 0xFF);
}
unsigned char patternMid(int i) {
    return static_cast<unsigned char>((i * 5 + 2) & 0xFF);
}
unsigned char patternHigh(int i) {
    return static_cast<unsigned char>((i * 11 + 3) & 0xFF);
}

TEST(CompanionWaveformMxwfTest, HeaderAndMaxMixedPayload) {
    // audioSampleRate>0 with maxVisualSamples set => "summary" path; a small
    // frameLength keeps getDataSize() small and even.
    Waveform waveform(44100, 4, 44100, 200, /*stemCount*/ 0);
    const int dataSize = waveform.getDataSize();
    ASSERT_GT(dataSize, 0);
    ASSERT_EQ(dataSize % 2, 0);
    const int frames = dataSize / 2;

    // Fill a deterministic, per-entry pattern (each interleaved L/R slot
    // distinct so the max-mix is observable).
    WaveformData* d = waveform.data();
    for (int i = 0; i < dataSize; ++i) {
        d[i].filtered.all = patternAll(i);
        d[i].filtered.low = patternLow(i);
        d[i].filtered.mid = patternMid(i);
        d[i].filtered.high = patternHigh(i);
    }

    const QByteArray blob = mixxx::companion::encodeWaveformSummaryMxwf(
            1234u, waveform, 5.0);

    ASSERT_EQ(blob.size(), 32 + frames * 4);

    QDataStream ds(blob);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);

    char magic[4];
    ds.readRawData(magic, 4);
    EXPECT_EQ(QByteArray(magic, 4), QByteArray("MXWF"));

    quint16 version = 0;
    quint16 flags = 0;
    ds >> version >> flags;
    EXPECT_EQ(version, 1);
    EXPECT_EQ(flags, 0);

    quint32 trackId = 0;
    ds >> trackId;
    EXPECT_EQ(trackId, 1234u);

    float duration = 0.0f;
    ds >> duration;
    EXPECT_FLOAT_EQ(duration, 5.0f);

    quint32 sampleCount = 0;
    ds >> sampleCount;
    EXPECT_EQ(sampleCount, static_cast<quint32>(frames));

    quint8 channels = 0;
    quint8 bands = 0;
    ds >> channels >> bands;
    EXPECT_EQ(channels, 1);
    EXPECT_EQ(bands, 4);

    quint16 reserved16 = 0xFFFF;
    quint64 reserved64 = 0xFFFF;
    ds >> reserved16 >> reserved64;
    EXPECT_EQ(reserved16, 0);
    EXPECT_EQ(reserved64, 0u);

    auto readByte = [&ds]() {
        quint8 b = 0;
        ds >> b;
        return static_cast<int>(b);
    };
    auto maxOf = [](unsigned char a, unsigned char b) {
        return static_cast<int>(a > b ? a : b);
    };

    for (int f = 0; f < frames; ++f) {
        const int l = 2 * f;
        const int r = l + 1;
        // Order is [all, low, mid, high], each = max(Left, Right).
        EXPECT_EQ(readByte(), maxOf(patternAll(l), patternAll(r))) << "all f=" << f;
        EXPECT_EQ(readByte(), maxOf(patternLow(l), patternLow(r))) << "low f=" << f;
        EXPECT_EQ(readByte(), maxOf(patternMid(l), patternMid(r))) << "mid f=" << f;
        EXPECT_EQ(readByte(), maxOf(patternHigh(l), patternHigh(r)))
                << "high f=" << f;
    }
}

TEST(CompanionWaveformMxwfTest, EmptyWaveformIsHeaderOnly) {
    // audioSampleRate == 0 => numberOfVisualSamples stays 0 => getDataSize()==0.
    Waveform waveform(0, 0, 0, 0, 0);
    ASSERT_EQ(waveform.getDataSize(), 0);
    const QByteArray blob = mixxx::companion::encodeWaveformSummaryMxwf(
            7u, waveform, 0.0);
    ASSERT_EQ(blob.size(), 32);
    EXPECT_EQ(blob.left(4), QByteArray("MXWF"));
}

} // namespace
