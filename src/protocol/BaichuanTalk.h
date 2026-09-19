#pragma once

#include "protocol/BaichuanControl.h"

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>

// Two-way talk over Baichuan (TCP 9000): the client speaks into the camera's
// speaker. The exchange, on one authenticated connection:
//
//   cmd 10   TalkAbility   camera -> the audio formats it will accept
//   cmd 201  TalkConfig    client -> the one format we will send
//   cmd 202  Talk          client -> ADPCM blocks, wrapped as BCMedia audio frames
//   cmd 11   TalkReset     client -> end the session (also clears a stale one)
//
// The wire flow and framing were learned from public protocol documentation and
// checked against the message shapes this client already speaks; the code here is
// independent. Cameras take IMA/DVI-4 ADPCM (mono); the sample rate and block size
// come from TalkAbility rather than being assumed.
namespace rl {

// What a camera will accept, and so what we send.
struct TalkFormat {
    QString duplex = QStringLiteral("FDX");
    QString streamMode = QStringLiteral("followVideoStream");
    QString audioType = QStringLiteral("adpcm");
    int sampleRate = 16000;
    int samplePrecision = 16;
    int lengthPerEncoder = 1024; // samples per block; ADPCM data bytes = half of this
    QString soundTrack = QStringLiteral("mono");

    int dataBytes() const { return lengthPerEncoder / 2; }
};

namespace talk {

constexpr quint32 kCmdAbility = 10;
constexpr quint32 kCmdReset = 11;
constexpr quint32 kCmdConfig = 201;
constexpr quint32 kCmdData = 202;

// Pick the format to send from a TalkAbility reply: ADPCM mono if offered (the only
// codec cameras are known to take), FDX duplex and followVideoStream when listed.
// nullopt when the camera offers no ADPCM.
std::optional<TalkFormat> parseAbility(const QByteArray &abilityXml);

// The TalkConfig body that selects `format` for `channel`.
QByteArray configXml(int channel, const TalkFormat &format);

// One ADPCM block (4-byte header + data) as a BCMedia audio frame, ready to be the
// payload of a cmd-202 message.
QByteArray frameAdpcm(const QByteArray &block);

} // namespace talk

// One talk session. Blocking; run it on a worker thread.
class BaichuanTalk
{
public:
    struct Params {
        QString host;
        int port = 9000;
        QString username;
        QString password;
        int channel = 0;
    };

    explicit BaichuanTalk(Params params);
    ~BaichuanTalk();

    // Connect, log in, read the camera's talk ability and select a format. On
    // failure returns false with a user-presentable reason in *error.
    bool open(QString *error);
    // Send one ADPCM block. False once the camera stops accepting the stream.
    bool send(const QByteArray &adpcmBlock, QString *error = nullptr);
    // End the session on the device and drop the connection. Safe to call twice.
    void close();

    const TalkFormat &format() const { return m_format; }
    bool isOpen() const { return m_talking; }

private:
    Params m_p;
    std::unique_ptr<BaichuanControl> m_ctl;
    TalkFormat m_format;
    quint32 m_dataMessId = 0;
    bool m_talking = false;
};

} // namespace rl
