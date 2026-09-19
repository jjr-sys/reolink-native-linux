#pragma once

#include <QByteArray>
#include <QList>

struct SwrContext;

namespace rl {

// Microphone audio -> the ADPCM the camera's speaker expects (two-way talk).
//
// Three small stages, each usable and testable on its own:
//   MicResampler   capture format (whatever the sound card gives) -> mono S16 @ camera rate
//   ImaAdpcm       mono S16 -> IMA/DVI-4 ADPCM blocks (the codec Baichuan talk carries)
//   TalkEncoder    the two chained, emitting whole blocks as PCM arrives

// The capture side's sample layouts we accept. Mirrors QAudioFormat::SampleFormat
// without pulling Qt Multimedia into this header.
enum class MicSampleFormat { UInt8, Int16, Int32, Float };

// Converts raw capture bytes to mono signed 16-bit at a fixed output rate.
// Not thread-safe.
class MicResampler
{
public:
    MicResampler(int inRate, int inChannels, MicSampleFormat inFormat, int outRate);
    ~MicResampler();
    MicResampler(const MicResampler &) = delete;
    MicResampler &operator=(const MicResampler &) = delete;

    bool isValid() const { return m_swr != nullptr; }
    // `bytes` must be whole frames (channels * sample width). Returns mono S16 LE PCM.
    QByteArray convert(const QByteArray &bytes);

private:
    SwrContext *m_swr = nullptr;
    int m_inChannels = 0;
    int m_inBytesPerFrame = 0;
    int m_outRate = 0;
    int m_inRate = 0;
};

// IMA/DVI-4 ADPCM encoder in the block layout the cameras use: each block is a
// 4-byte header (predictor s16 LE, step index, reserved 0) followed by
// `dataBytes` of 4-bit codes, low nibble first. The header carries the first
// sample, so a block covers dataBytes*2 + 1 input samples. The predictor/index
// state runs across blocks, but each block header re-states it so a receiver
// can start at any block.
class ImaAdpcm
{
public:
    // dataBytes: ADPCM payload bytes per block (the camera's lengthPerEncoder / 2).
    explicit ImaAdpcm(int dataBytes);

    int dataBytes() const { return m_dataBytes; }
    int samplesPerBlock() const { return m_dataBytes * 2 + 1; }
    int blockBytes() const { return m_dataBytes + 4; }

    // Encode exactly samplesPerBlock() mono S16 samples into one block.
    QByteArray encodeBlock(const qint16 *samples);

    // Reference decoder for one block (used by tests, and available for cameras
    // that send ADPCM audio back). Appends samplesPerBlock() samples.
    static QList<qint16> decodeBlock(const QByteArray &block);

private:
    int m_dataBytes;
    int m_predictor = 0;
    int m_index = 0;
};

// Buffers PCM and yields complete ADPCM blocks. Mono S16 in.
class TalkEncoder
{
public:
    explicit TalkEncoder(int dataBytes) : m_adpcm(dataBytes) {}

    int blockBytes() const { return m_adpcm.blockBytes(); }
    int samplesPerBlock() const { return m_adpcm.samplesPerBlock(); }

    // Append mono S16 PCM; returns every block that is now complete.
    QList<QByteArray> push(const QByteArray &monoS16);

    // Duration one block represents, in ms, for a given sample rate.
    int blockMs(int sampleRate) const { return samplesPerBlock() * 1000 / sampleRate; }

private:
    ImaAdpcm m_adpcm;
    QByteArray m_pending;
};

} // namespace rl
