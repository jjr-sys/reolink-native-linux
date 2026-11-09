#pragma once

#include "media/BcMediaParser.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

struct AVFormatContext;

// Writes the video and AAC audio that BcMediaParser pulls out of a Baichuan media stream
// into an MP4 file, stream copy (no re-encode). Video times come from the camera's
// microsecond clock; AAC frames carry no time of their own, so audio is laid end to end
// (1024 samples a frame) from the video moment its first frame arrived at.
//
// The file starts at the first key frame. Nothing is written until the streams are known
// (the first audio frame, or two seconds of video without one), so the header can declare
// both. Destroyed without finish(), it removes its partial file.
namespace rl {

struct AdtsHeader {
    int headerLen = 0;   // 7, or 9 with a CRC
    int objectType = 0;  // MPEG-4 audio object type (2 = AAC LC)
    int srIndex = 0;
    int sampleRate = 0;
    int channels = 0;
    int frameLen = 0;    // whole frame, header included
};
std::optional<AdtsHeader> parseAdts(const QByteArray &frame);
// The two-byte AudioSpecificConfig an MP4 'esds' needs for this stream.
QByteArray aacAudioSpecificConfig(const AdtsHeader &h);

// The parameter-set NAL units (H.264 SPS/PPS; H.265 VPS/SPS/PPS) of an Annex-B access
// unit, each with a 4-byte start code: the stream's extradata.
QByteArray annexBParameterSets(const QByteArray &annexB, BcMediaParser::Codec codec);

// Turns the camera's 32-bit microsecond counter (wraps every ~71.6 min) into a running
// time from its first reading.
class MicrosClock
{
public:
    qint64 push(quint32 us);
private:
    bool m_started = false;
    quint32 m_last = 0;
    qint64 m_total = 0;
};

class BcMp4Writer
{
public:
    explicit BcMp4Writer(QString path);
    ~BcMp4Writer();
    BcMp4Writer(const BcMp4Writer &) = delete;
    BcMp4Writer &operator=(const BcMp4Writer &) = delete;

    // `width`/`height`: the stream's declared size (BcMediaParser::declaredWidth/Height).
    void addVideo(const BcMediaParser::VideoFrame &f, int width, int height);
    void addAudio(const QByteArray &adts);
    // Write the index and close. False if no video was written or the file failed.
    bool finish(QString *error = nullptr);

    bool failed() const { return !m_error.isEmpty(); }
    QString error() const { return m_error; }
    qint64 durationMs() const { return m_lastVideoUs / 1000; }
    int videoFrames() const { return m_videoFrames; }
    int audioFrames() const { return m_audioFrames; }

private:
    struct Pending {
        bool video = true;
        bool key = false;
        qint64 us = 0;     // video: frame time; audio: unused (counted in samples)
        QByteArray data;   // video: Annex-B access unit; audio: raw AAC (no ADTS header)
    };
    bool open();
    void writeVideo(const Pending &p);
    void writeAudio(const QByteArray &raw);
    void fail(const QString &why);
    void closeFile();

    QString m_path;
    QString m_error;
    AVFormatContext *m_fmt = nullptr;
    bool m_headerWritten = false;
    bool m_finished = false;

    BcMediaParser::Codec m_codec = BcMediaParser::Codec::Unknown;
    QByteArray m_videoExtra;
    int m_width = 0, m_height = 0;
    MicrosClock m_clock;
    bool m_started = false;   // first key frame seen
    qint64 m_lastVideoUs = 0;
    qint64 m_lastVideoDts = -1;

    std::optional<AdtsHeader> m_aac;
    qint64 m_audioStartUs = -1;  // video time the first audio frame lines up with
    qint64 m_audioSamples = 0;

    std::vector<Pending> m_pending;  // held until the header can be written
    int m_videoFrames = 0, m_audioFrames = 0;
};

} // namespace rl
