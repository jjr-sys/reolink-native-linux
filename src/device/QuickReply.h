#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QXmlStreamReader>

#include <nlohmann/json.hpp>

// Doorbell quick-reply clips: pure parsing/formatting, no I/O. The device lists its
// clips over HTTP-CGI (GetAudioFileList) or Baichuan (cmd 347) and plays one over
// QuickReplyPlay / Baichuan cmd 349 (clean-room from the MIT reolink_aio; see
// wayfinder/research/03-doorbell-quick-replies.md). Some NVRs answer the HTTP list
// with nothing while Baichuan answers, so callers try both.
namespace rl::quickreply {

struct Clip {
    int id = 0;
    QString name;
};

struct ClipList {
    QVector<Clip> clips;
    int maxFiles = 0; // device's clip capacity when it reports one (Baichuan), else 0
};

inline constexpr quint32 kBcListCmd = 347;
inline constexpr quint32 kBcPlayCmd = 349;

// HTTP rspCodes after which reolink_aio retries the same command over Baichuan.
inline bool httpShouldFallBack(int rspCode)
{
    return rspCode == -4 || rspCode == -9 || rspCode == -12 || rspCode == -13 || rspCode == -17;
}

// `value` of a GetAudioFileList reply: {"AudioFileList": [{"id":..,"fileName":..}]}.
// The list may be null or absent. Entries without an id are dropped.
inline ClipList parseHttpList(const nlohmann::json &value)
{
    ClipList out;
    if (!value.is_object() || !value.contains("AudioFileList") || !value["AudioFileList"].is_array())
        return out;
    for (const auto &e : value["AudioFileList"]) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_number_integer())
            continue;
        Clip c;
        c.id = e["id"].get<int>();
        c.name = e.contains("fileName") && e["fileName"].is_string()
                     ? QString::fromStdString(e["fileName"].get<std::string>())
                     : QString();
        out.clips.append(c);
    }
    return out;
}

// Baichuan cmd 347 reply: <audioFileInfoList><maxFileNumber/>...<audioFileInfo><id/><fileName/>...
inline ClipList parseBaichuanList(const QByteArray &xml)
{
    ClipList out;
    QXmlStreamReader r(xml);
    bool inInfo = false;
    Clip cur;
    bool haveId = false;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            const QStringView n = r.name();
            if (n == u"audioFileInfo") {
                inInfo = true;
                cur = Clip();
                haveId = false;
            } else if (n == u"maxFileNumber") {
                out.maxFiles = r.readElementText().toInt();
            } else if (inInfo && n == u"id") {
                cur.id = r.readElementText().toInt(&haveId);
            } else if (inInfo && n == u"fileName") {
                cur.name = r.readElementText();
            }
        } else if (r.isEndElement() && r.name() == u"audioFileInfo") {
            if (inInfo && haveId)
                out.clips.append(cur);
            inInfo = false;
        }
    }
    return out;
}

// Baichuan cmd 349 request body. Channel is the 0-based NVR channel.
inline QByteArray baichuanPlayBody(int channel, int fileId)
{
    return QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
                             "<audioFileInfo version=\"1.1\">\n<channelId>")
           + QByteArray::number(channel) + QByteArrayLiteral("</channelId>\n<id>")
           + QByteArray::number(fileId)
           + QByteArrayLiteral("</id>\n<timeout>0</timeout>\n</audioFileInfo>\n</body>\n");
}

// Baichuan status codes that mean the play was accepted.
inline bool baichuanStatusOk(quint16 status)
{
    return status == 200 || status == 201 || status == 300;
}

// Auto-reply: play a clip by itself when the doorbell is pressed and nobody answers.
struct AutoReply {
    bool valid = false;
    bool enable = false;
    int fileId = -1;   // -1 = no clip chosen
    int timeout = 15;  // seconds to wait for a person before replying
};

inline constexpr quint32 kBcAutoReplyGetCmd = 427;
inline constexpr quint32 kBcAutoReplySetCmd = 428;

// `value` of GetAutoReply: {"AutoReply": {"enable":0,"fileId":-1,"timeout":15}}.
inline AutoReply parseHttpAutoReply(const nlohmann::json &value)
{
    AutoReply a;
    if (!value.is_object() || !value.contains("AutoReply") || !value["AutoReply"].is_object())
        return a;
    const auto &j = value["AutoReply"];
    a.valid = true;
    a.enable = j.contains("enable") && j["enable"].is_number() && j["enable"].get<int>() != 0;
    if (j.contains("fileId") && j["fileId"].is_number_integer())
        a.fileId = j["fileId"].get<int>();
    if (j.contains("timeout") && j["timeout"].is_number_integer())
        a.timeout = j["timeout"].get<int>();
    return a;
}

// Baichuan cmd 427 reply: <enable>, <audioId> (the file id), <timeout>.
inline AutoReply parseBaichuanAutoReply(const QByteArray &xml)
{
    AutoReply a;
    QXmlStreamReader r(xml);
    bool haveEnable = false, haveAudio = false, haveTimeout = false; // first occurrence wins,
    while (!r.atEnd()) {                                             // like the write side
        r.readNext();
        if (!r.isStartElement())
            continue;
        const QStringView n = r.name();
        if (n == u"enable" && !haveEnable) {
            a.enable = r.readElementText().toInt() != 0;
            haveEnable = true;
        } else if (n == u"audioId" && !haveAudio) {
            a.fileId = r.readElementText().toInt();
            haveAudio = true;
        } else if (n == u"timeout" && !haveTimeout) {
            a.timeout = r.readElementText().toInt();
            haveTimeout = true;
        }
    }
    const bool any = haveEnable || haveAudio || haveTimeout;
    a.valid = any;
    return a;
}

// SetAutoReply `param`: note the wrapper object, unlike the flat QuickReplyPlay.
inline nlohmann::json httpAutoReplyParam(int channel, const AutoReply &a)
{
    return nlohmann::json{{"AutoReply",
                           {{"channel", channel},
                            {"enable", a.enable ? 1 : 0},
                            {"fileId", a.fileId},
                            {"timeout", a.timeout}}}};
}

// Fields to write into the cmd 427 config before sending it back as cmd 428.
inline QVariantMap baichuanAutoReplyChanges(const AutoReply &a)
{
    return {{QStringLiteral("enable"), a.enable ? 1 : 0},
            {QStringLiteral("audioId"), a.fileId},
            {QStringLiteral("timeout"), a.timeout}};
}

} // namespace rl::quickreply
