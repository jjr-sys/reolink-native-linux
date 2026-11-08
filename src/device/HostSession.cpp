#include "HostSession.h"

#include "core/Backoff.h"
#include "core/Log.h"
#include "protocol/BaichuanChannels.h"

#include <QCoreApplication>
#include <algorithm>
#include <QElapsedTimer>

namespace rl::device {

// Unreachable-host probe: seconds, doubling 15..300.
static constexpr BackoffPolicy kHostRetry{kFirstRetrySecs, kMaxRetrySecs};

// ---- HostSession -----------------------------------------------------------------------

bool HostSession::beginValidation()
{
    if (m_validating)
        return false;
    m_validating = true;
    return true;
}

void HostSession::endValidation() { m_validating = false; }

void HostSession::keepRetrying(const QDateTime &now)
{
    m_nextRetryAt = now.addSecs(m_retryDelaySecs);
    m_retryDelaySecs = kHostRetry.grow(m_retryDelaySecs);
}

void HostSession::validationFailed(Problem problem, const QDateTime &now)
{
    if (problem == Problem::Unreachable) {
        keepRetrying(now);
        return;
    }
    // Auth / Locked (and anything else that is not plain unreachability) wait for the
    // user: every rejected login burns the firmware's 10-attempt lockout counter.
    validationSucceeded();
}

void HostSession::validationSucceeded()
{
    m_nextRetryAt.reset();
    m_retryDelaySecs = kFirstRetrySecs;
}

bool HostSession::retryDue(const QDateTime &now) const
{
    return m_nextRetryAt && *m_nextRetryAt <= now;
}

bool HostSession::takeRetryIfDue(const QDateTime &now)
{
    if (!retryDue(now))
        return false;
    m_nextRetryAt.reset(); // the delay stays: a failing attempt backs off from where it was
    return true;
}

bool HostSession::beginPoll()
{
    if (m_polling)
        return false;
    m_polling = true;
    return true;
}

void HostSession::endPoll() { m_polling = false; }

HostSession::PollAction HostSession::onPoll(bool transportOk)
{
    if (!transportOk) {
        ++m_pollFails;
        if (m_pollFails == kPollFailuresBeforeUnreachable || (m_apiOnly && m_pollFails % 6 == 0))
            return PollAction::ProbeAlive;
        return PollAction::None;
    }
    PollAction action = PollAction::None;
    if (m_apiOnly) {
        m_apiOnly = false;
        action = PollAction::ApiBack;
    } else if (m_pollFails >= kPollFailuresBeforeUnreachable) {
        action = PollAction::Recovered;
    }
    m_pollFails = 0;
    return action;
}

bool HostSession::beginProbe()
{
    if (m_probing)
        return false;
    m_probing = true;
    return true;
}

HostSession::ProbeAction HostSession::probeFinished(bool alive)
{
    m_probing = false;
    if (m_pollFails < kPollFailuresBeforeUnreachable)
        return ProbeAction::Ignore; // the API came back while we were checking
    if (!alive) {
        m_apiOnly = false;
        return ProbeAction::MarkUnreachable;
    }
    if (m_apiOnly)
        return ProbeAction::Ignore;
    m_apiOnly = true;
    return ProbeAction::EnterApiOnly;
}

QVector<qint64> HostSessions::takeDueRetries(const QDateTime &now)
{
    QVector<qint64> due;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
        if (it.value().takeRetryIfDue(now))
            due.append(it.key());
    return due;
}

// ---- logging in --------------------------------------------------------------------------

// Firmware spells the codec several ways ("h265", "H265", "hevc") and newer
// builds sometimes omit the field entirely. Empty result = unknown, and an
// unknown codec is PROBED from the stream rather than assumed — assuming h264
// for an HEVC main stream is exactly issue #4.
QString normalizeCodec(const QString &vtype)
{
    const QString v = vtype.toLower();
    if (v.contains(QLatin1String("265")) || v.contains(QLatin1String("hevc")))
        return QStringLiteral("h265");
    if (v.contains(QLatin1String("264")) || v.contains(QLatin1String("avc")))
        return QStringLiteral("h264");
    return {};
}


namespace {
// Declared display size of a GetEnc stream ("mainStream"/"subStream"), e.g.
// 7680x2160. Empty if absent/zero. Used to detect transmitted-rotated streams.
QSize encStreamSize(const Json &enc, const char *stream)
{
    const Json s = jsonObj(enc, stream);
    const int w = jsonInt(s, "width", 0);
    const int h = jsonInt(s, "height", 0);
    return (w > 0 && h > 0) ? QSize(w, h) : QSize();
}

// Configured bitrate of a GetEnc stream in kbps; 0 if absent.
int encStreamKbps(const Json &enc, const char *stream)
{
    return std::max(0, jsonInt(jsonObj(enc, stream), "bitRate", 0));
}

// The NVR's cameras as listed by its Baichuan port, for when its HTTP API is not answering.
// Blocking; call from a worker. Empty on any failure.
struct BaichuanCameras {
    QVector<int> channels;
    QHash<int, QString> names; // channel -> camera name; a channel with no name is absent
};

BaichuanCameras discoverCamerasBaichuan(const HostRecord &rec, const QString &password,
                                          const BaichuanOpener &baichuan)
{
    BaichuanParams p;
    p.host = rec.addr;
    p.port = 9000;
    p.username = rec.username;
    p.password = password;
    const std::unique_ptr<BaichuanLink> link = baichuan(p);
    BaichuanLink &control = *link;
    BaichuanCameras out;
    if (!control.open())
        return out;
    out.channels = bc::channelsFromStreamInfo(control.get(146, -1)); // StreamInfoList, host level
    // Names: one OSD read (cmd 44) per camera, as the official apps do. The NVR drops the
    // session on a channel it cannot answer (a third-party camera), so reconnect after one.
    // Bounded: this runs on a worker that app exit waits for, and one read can block for
    // seconds when an NVR is unwell. Channels not reached keep their "Channel N" label.
    QElapsedTimer budget;
    budget.start();
    for (int ch : out.channels) {
        if (budget.elapsed() > 8000)
            break;
        if (!control.isOpen() && !control.open())
            break;
        quint16 status = 0;
        const QString name = bc::osdChannelName(control.get(44, ch, &status));
        if (!name.isEmpty())
            out.names.insert(ch, name);
        else if (status != 200) {
            control.close(); // its session is unusable now; the next channel reopens
        }
    }
    control.close();
    return out;
}

} // namespace

Validation validateReolinkHost(ReolinkTransport &http, const HostRecord &rec,
                               const QString &password, bool alreadyDegraded,
                               const BaichuanOpener &baichuan)
{
    // Phase 1: device info, ch0 encoding, abilities, battery, and the NVR
    // channel list.
    const api::BatchResult b1 = http.call(Json::array({
        api::command(QStringLiteral("GetDevInfo")),
        api::command(QStringLiteral("GetEnc"), Json{{"channel", 0}}, 1),
        api::command(QStringLiteral("GetAbility"),
                     Json{{"User", {{"userName", rec.username.toStdString()}}}}),
        api::command(QStringLiteral("GetBatteryInfo"), Json{{"channel", 0}}),
        api::command(QStringLiteral("GetChannelstatus")),
    }));

    Validation v;
    v.password = password;
    v.status = QCoreApplication::translate("rl::DeviceManager", "unreachable");
    api::Capabilities caps;
    QVector<api::ChannelInfo> channels;
    QString ch0codec = QStringLiteral("h264");
    QSize ch0MainSize, ch0SubSize;
    int ch0MainKbps = 0, ch0SubKbps = 0;

    if (b1.transportOk) {
        for (const api::CommandResult &r : b1.results) {
            if (r.cmd == QLatin1String("GetDevInfo") && r.ok) {
                const Json info = jsonObj(r.value, "DevInfo");
                v.hostName = QString::fromStdString(jsonStr(info, "name"));
                v.model = QString::fromStdString(jsonStr(info, "model"));
                v.channelNum = jsonInt(info, "channelNum", 1);
                v.online = true;
                v.status = QCoreApplication::translate("rl::DeviceManager", "online");
                } else if (r.cmd == QLatin1String("GetEnc") && r.ok) {
                const Json enc = jsonObj(r.value, "Enc");
                ch0codec = normalizeCodec(QString::fromStdString(
                    jsonStr(jsonObj(enc, "mainStream"), "vType")));
                ch0MainSize = encStreamSize(enc, "mainStream");
                ch0SubSize = encStreamSize(enc, "subStream");
                ch0MainKbps = encStreamKbps(enc, "mainStream");
                ch0SubKbps = encStreamKbps(enc, "subStream");
            } else if (r.cmd == QLatin1String("GetAbility") && r.ok) {
                caps = api::parseAbility(r.value);
            } else if (r.cmd == QLatin1String("GetBatteryInfo") && r.ok) {
                v.battery = api::parseBatteryInfo(r.value);
            } else if (r.cmd == QLatin1String("GetChannelstatus") && r.ok) {
                channels = api::parseChannelStatus(r.value);
            }
        }
    }
    if (!v.online) {
        if (!b1.error.isEmpty())
            v.status = b1.error;
        // The client knows WHY: transport failures are retried with
        // backoff; auth failures wait for the user (lockout counter).
        switch (http.lastFailKind()) {
        case ReolinkTransport::FailKind::Auth: v.problem = Problem::Auth; break;
        case ReolinkTransport::FailKind::Locked: v.problem = Problem::Locked; break;
        default: v.problem = Problem::Unreachable; break;
        }
        // The HTTP API can hang while Baichuan (port 9000) still serves. If it does,
        // list the cameras from there so live view works; HTTP keeps being retried.
        if (v.problem == Problem::Unreachable && !alreadyDegraded && !password.isEmpty()) {
            const BaichuanCameras cams = discoverCamerasBaichuan(rec, password, baichuan);
            const QVector<int> &found = cams.channels;
            if (!found.isEmpty()) {
                qCInfo(lcCore) << rec.addr << "HTTP API unreachable; cameras listed via Baichuan:"
                               << found.size();
                v.degraded = true;
                v.model = rec.model;
                v.status = QCoreApplication::translate("rl::DeviceManager", "limited: device API not responding");
                for (int ch : found) {
                    ChannelResult cr;
                    cr.channel = ch;
                    cr.name = cams.names.value(ch, QCoreApplication::translate("rl::DeviceManager", "Channel %1").arg(ch + 1));
                    cr.online = true;
                    v.channels.append(cr);
                }
            }
        }
        return v;
    }
    v.problem = Problem::None;
    v.isAdmin = caps.isAdmin;

    auto capsFor = [&](int ch) {
        return ch >= 0 && ch < caps.channels.size() ? caps.channels[ch] : api::ChannelCaps{};
    };

    if (v.channelNum <= 1) {
        // Standalone camera: a single channel 0.
        ChannelResult cr;
        cr.channel = 0;
        cr.name = v.hostName;
        cr.online = true;
        cr.codec = ch0codec;
        cr.mainSize = ch0MainSize;
        cr.subSize = ch0SubSize;
        cr.mainKbps = ch0MainKbps;
        cr.subKbps = ch0SubKbps;
        cr.caps = capsFor(0);
        v.talk = cr.caps.talk;
        v.channels.append(cr);
    } else {
        // NVR: one entry per ONLINE channel. Phase 2 fetches each channel's
        // main-stream codec (main can be h265 while sub is h264).
        QVector<api::ChannelInfo> online;
        for (const api::ChannelInfo &c : channels)
            if (c.online)
                online.append(c);

        QHash<int, QString> codecByChannel;
        QHash<int, QSize> mainSizeByChannel;
        QHash<int, QSize> subSizeByChannel;
        QHash<int, int> mainKbpsByChannel, subKbpsByChannel;
        if (!online.isEmpty()) {
            Json encCmds = Json::array();
            for (const api::ChannelInfo &c : online)
                encCmds.push_back(
                    api::command(QStringLiteral("GetEnc"), Json{{"channel", c.channel}}, 1));
            const api::BatchResult b2 = http.call(encCmds);
            if (b2.transportOk)
                for (const api::CommandResult &r : b2.results)
                    if (r.cmd == QLatin1String("GetEnc") && r.ok) {
                        const Json enc = jsonObj(r.value, "Enc");
                        const int ch = jsonInt(enc, "channel", 0);
                        codecByChannel[ch] = normalizeCodec(QString::fromStdString(
                            jsonStr(jsonObj(enc, "mainStream"), "vType")));
                        mainSizeByChannel[ch] = encStreamSize(enc, "mainStream");
                        subSizeByChannel[ch] = encStreamSize(enc, "subStream");
                        mainKbpsByChannel[ch] = encStreamKbps(enc, "mainStream");
                        subKbpsByChannel[ch] = encStreamKbps(enc, "subStream");
                    }
        }
        for (const api::ChannelInfo &c : online) {
            ChannelResult cr;
            cr.channel = c.channel;
            cr.name = c.name;
            cr.online = true;
            cr.codec = codecByChannel.value(c.channel, QStringLiteral("h264"));
            cr.mainSize = mainSizeByChannel.value(c.channel);
            cr.subSize = subSizeByChannel.value(c.channel);
            cr.mainKbps = mainKbpsByChannel.value(c.channel);
            cr.subKbps = subKbpsByChannel.value(c.channel);
            cr.uid = c.uid;
            cr.caps = capsFor(c.channel);
            v.talk = v.talk || cr.caps.talk;
            v.channels.append(cr);
        }
        if (v.channels.isEmpty()) {
            // NVR reachable but no online cameras yet — keep a placeholder.
            ChannelResult cr;
            cr.channel = 0;
            cr.name = v.hostName;
            cr.online = false;
            v.channels.append(cr);
        }
    }
    return v;
}

} // namespace rl::device
