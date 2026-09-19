#include "TalkCodec.h"

#include "core/Log.h"

#include <algorithm>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace rl {

namespace {

// The standard IMA ADPCM tables.
constexpr int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

AVSampleFormat toAvFormat(MicSampleFormat f)
{
    switch (f) {
    case MicSampleFormat::UInt8:
        return AV_SAMPLE_FMT_U8;
    case MicSampleFormat::Int16:
        return AV_SAMPLE_FMT_S16;
    case MicSampleFormat::Int32:
        return AV_SAMPLE_FMT_S32;
    case MicSampleFormat::Float:
        return AV_SAMPLE_FMT_FLT;
    }
    return AV_SAMPLE_FMT_S16;
}

int bytesPerSample(MicSampleFormat f)
{
    switch (f) {
    case MicSampleFormat::UInt8:
        return 1;
    case MicSampleFormat::Int16:
        return 2;
    case MicSampleFormat::Int32:
    case MicSampleFormat::Float:
        return 4;
    }
    return 2;
}

int clampInt(int v, int lo, int hi)
{
    return std::min(std::max(v, lo), hi);
}

// The reconstruction step every IMA decoder in practice (libavcodec, and so the
// players and cameras that follow it) applies: ((2*|code|+1) * step) >> 3. It differs
// from the textbook sum-of-shifts by a rounding LSB, so the encoder must track the
// predictor with this exact form or it would drift from what the decoder rebuilds.
int reconstructDelta(int code, int step)
{
    return ((2 * (code & 7) + 1) * step) >> 3;
}

// One IMA step: quantise `sample` against the running predictor, update the
// predictor/index exactly as a decoder will, and return the 4-bit code.
int encodeSample(int sample, int &predictor, int &index)
{
    const int step = kStepTable[index];
    int diff = sample - predictor;
    int code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int s = step;
    if (diff >= s) {
        code |= 4;
        diff -= s;
    }
    s >>= 1;
    if (diff >= s) {
        code |= 2;
        diff -= s;
    }
    s >>= 1;
    if (diff >= s)
        code |= 1;
    const int delta = reconstructDelta(code, step);
    predictor = clampInt(code & 8 ? predictor - delta : predictor + delta, -32768, 32767);
    index = clampInt(index + kIndexTable[code], 0, 88);
    return code;
}

} // namespace

// ---- MicResampler -----------------------------------------------------------

MicResampler::MicResampler(int inRate, int inChannels, MicSampleFormat inFormat, int outRate)
    : m_inChannels(inChannels), m_inBytesPerFrame(inChannels * bytesPerSample(inFormat)),
      m_outRate(outRate), m_inRate(inRate)
{
    if (inRate <= 0 || inChannels <= 0 || outRate <= 0)
        return;
    AVChannelLayout in{};
    av_channel_layout_default(&in, inChannels);
    AVChannelLayout out{};
    av_channel_layout_default(&out, 1); // mono
    const int rc = swr_alloc_set_opts2(&m_swr, &out, AV_SAMPLE_FMT_S16, outRate, &in,
                                       toAvFormat(inFormat), inRate, 0, nullptr);
    av_channel_layout_uninit(&in);
    av_channel_layout_uninit(&out);
    if (rc < 0 || swr_init(m_swr) < 0) {
        qCWarning(lcMedia) << "talk: cannot resample microphone" << inRate << "Hz," << inChannels
                           << "ch";
        swr_free(&m_swr);
    }
}

MicResampler::~MicResampler()
{
    swr_free(&m_swr);
}

QByteArray MicResampler::convert(const QByteArray &bytes)
{
    if (!m_swr || bytes.size() < m_inBytesPerFrame)
        return {};
    const int inFrames = bytes.size() / m_inBytesPerFrame;
    const int maxOut = swr_get_out_samples(m_swr, inFrames);
    if (maxOut <= 0)
        return {};
    QByteArray out(maxOut * 2, Qt::Uninitialized);
    auto *dst = reinterpret_cast<uint8_t *>(out.data());
    const auto *src = reinterpret_cast<const uint8_t *>(bytes.constData());
    const int got = swr_convert(m_swr, &dst, maxOut, &src, inFrames);
    if (got <= 0)
        return {};
    out.resize(got * 2);
    return out;
}

// ---- ImaAdpcm ---------------------------------------------------------------

ImaAdpcm::ImaAdpcm(int dataBytes) : m_dataBytes(dataBytes) {}

QByteArray ImaAdpcm::encodeBlock(const qint16 *samples)
{
    QByteArray block;
    block.reserve(blockBytes());
    // Header: the first sample becomes the predictor. The step index carries over
    // from the previous block so quantisation stays continuous.
    m_predictor = samples[0];
    block.append(char(m_predictor & 0xff));
    block.append(char((m_predictor >> 8) & 0xff));
    block.append(char(m_index));
    block.append(char(0));
    // Then dataBytes*2 codes, two per byte, low nibble first.
    for (int i = 0; i < m_dataBytes; ++i) {
        const int lo = encodeSample(samples[1 + 2 * i], m_predictor, m_index);
        const int hi = encodeSample(samples[2 + 2 * i], m_predictor, m_index);
        block.append(char(lo | (hi << 4)));
    }
    return block;
}

QList<qint16> ImaAdpcm::decodeBlock(const QByteArray &block)
{
    QList<qint16> out;
    if (block.size() < 5)
        return out;
    const auto *b = reinterpret_cast<const uchar *>(block.constData());
    int predictor = static_cast<qint16>(b[0] | (b[1] << 8));
    int index = clampInt(b[2], 0, 88);
    out.append(static_cast<qint16>(predictor));
    for (int i = 4; i < block.size(); ++i) {
        for (const int code : {b[i] & 0x0f, b[i] >> 4}) {
            const int delta = reconstructDelta(code, kStepTable[index]);
            predictor = clampInt(code & 8 ? predictor - delta : predictor + delta, -32768, 32767);
            index = clampInt(index + kIndexTable[code], 0, 88);
            out.append(static_cast<qint16>(predictor));
        }
    }
    return out;
}

// ---- TalkEncoder ------------------------------------------------------------

QList<QByteArray> TalkEncoder::push(const QByteArray &monoS16)
{
    QList<QByteArray> blocks;
    m_pending.append(monoS16);
    const int need = samplesPerBlock() * 2;
    while (m_pending.size() >= need) {
        blocks.append(m_adpcm.encodeBlock(reinterpret_cast<const qint16 *>(m_pending.constData())));
        m_pending.remove(0, need);
    }
    return blocks;
}

} // namespace rl
