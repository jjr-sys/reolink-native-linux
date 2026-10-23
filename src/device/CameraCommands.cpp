#include "CameraCommands.h"

namespace rl::device {

namespace {
// One short Baichuan session; closed on scope exit. Null when it could not be opened.
class Session
{
public:
    Session(const BaichuanOpener &open, const BaichuanParams &p) : m_link(open(p))
    {
        m_ok = m_link->open();
    }
    ~Session() { m_link->close(); }
    bool ok() const { return m_ok; }
    BaichuanLink &link() { return *m_link; }

private:
    std::unique_ptr<BaichuanLink> m_link;
    bool m_ok = false;
};

CommandResult failure(CommandFailure f, int status = 0, const QString &detail = {})
{
    CommandResult r;
    r.failure = f;
    r.status = status;
    r.detail = detail;
    return r;
}
CommandResult success()
{
    CommandResult r;
    r.ok = true;
    return r;
}
bool successStatus(quint16 st) { return st == 200 || st == 201 || st == 300; }
} // namespace

bool CameraCommands::alertCommandIds(const QString &kind, quint32 &getCmd, quint32 &setCmd)
{
    if (kind == QLatin1String("push")) { getCmd = 219; setCmd = 218; return true; }
    if (kind == QLatin1String("email")) { getCmd = 217; setCmd = 216; return true; }
    if (kind == QLatin1String("ftp")) { getCmd = 70; setCmd = 71; return true; }
    return false;
}

QVariantMap CameraCommands::fetchAlerts(const BaichuanParams &host, int channel) const
{
    QVariantMap m;
    Session s(m_open, host);
    if (!s.ok()) {
        m[QStringLiteral("ok")] = false;
        return m;
    }
    const int push = s.link().readEnable(219, channel);
    const int email = s.link().readEnable(217, channel);
    const int ftp = s.link().readEnable(70, channel);
    m[QStringLiteral("ok")] = (push >= 0 || email >= 0 || ftp >= 0);
    m[QStringLiteral("push")] = push;
    m[QStringLiteral("email")] = email;
    m[QStringLiteral("ftp")] = ftp;
    return m;
}

CommandResult CameraCommands::setAlert(const BaichuanParams &host, int channel,
                                       const QString &kind, bool enable) const
{
    quint32 getCmd = 0, setCmd = 0;
    if (!alertCommandIds(kind, getCmd, setCmd))
        return failure(CommandFailure::UnknownKind);
    Session s(m_open, host);
    if (!s.ok())
        return failure(CommandFailure::Unreachable);
    return s.link().writeEnable(getCmd, setCmd, channel, enable)
               ? success()
               : failure(CommandFailure::Rejected);
}

QVariantMap CameraCommands::parseRecSchedule(const QByteArray &xml)
{
    QVariantMap m;
    // <item><type>T</type><valueTable>V</valueTable></item> pairs.
    int pos = 0;
    while (true) {
        const int t0 = xml.indexOf("<type>", pos);
        if (t0 < 0)
            break;
        const int t1 = xml.indexOf("</type>", t0);
        const int v0 = xml.indexOf("<valueTable>", t1);
        const int v1 = xml.indexOf("</valueTable>", v0);
        if (t1 < 0 || v0 < 0 || v1 < 0)
            break;
        m.insert(QString::fromLatin1(xml.mid(t0 + 6, t1 - t0 - 6)),
                 QString::fromLatin1(xml.mid(v0 + 12, v1 - v0 - 12)));
        pos = v1;
    }
    const int e0 = xml.indexOf("<enable>");
    if (e0 >= 0)
        m.insert(QStringLiteral("enable"), xml.mid(e0 + 8, 1) == "1");
    return m;
}

QVariantMap CameraCommands::fetchRecSchedule(const BaichuanParams &host, int channel) const
{
    Session s(m_open, host);
    if (!s.ok())
        return {};
    return parseRecSchedule(s.link().get(81, channel));
}

CommandResult CameraCommands::writeRecSchedule(const BaichuanParams &host, int channel,
                                               const QString &type, const QString &table) const
{
    Session s(m_open, host);
    if (!s.ok())
        return failure(CommandFailure::Unreachable);
    const QByteArray xml = s.link().get(81, channel);
    const QByteArray typeTag = "<type>" + type.toLatin1() + "</type>";
    const int t = xml.indexOf(typeTag);
    const int v0 = t >= 0 ? xml.indexOf("<valueTable>", t) : -1;
    const int v1 = v0 >= 0 ? xml.indexOf("</valueTable>", v0) : -1;
    if (v1 <= 0)
        return failure(CommandFailure::NotFound);
    const QByteArray mutated = xml.left(v0 + 12) + table.toLatin1() + xml.mid(v1);
    quint16 st = 0;
    s.link().transact(82, channel, mutated, &st);
    return successStatus(st) ? success() : failure(CommandFailure::Rejected, st);
}

QVariantMap CameraCommands::fetchConfig(const BaichuanParams &host, int channel, quint32 cmdId,
                                        const QByteArray &reqBody) const
{
    Session s(m_open, host);
    return s.ok() ? s.link().getConfigFlat(cmdId, channel, reqBody) : QVariantMap{};
}

CommandResult CameraCommands::writeConfig(const BaichuanParams &host, int channel,
                                          quint32 getCmd, quint32 setCmd,
                                          const QVariantMap &changes,
                                          const QByteArray &reqBody) const
{
    Session s(m_open, host);
    if (!s.ok())
        return failure(CommandFailure::Unreachable);
    return s.link().writeFields(getCmd, setCmd, channel, changes, reqBody)
               ? success()
               : failure(CommandFailure::Rejected);
}

CommandResult CameraCommands::toggleFloodlight(ReolinkTransport &http, int channel)
{
    // Read the current config so the toggle flips only on/off and leaves the user's
    // brightness + auto/schedule mode intact.
    const api::CommandResult got =
        http.callOne(QStringLiteral("GetWhiteLed"), Json{{"channel", channel}}, /*action=*/1);
    if (!got.ok)
        return failure(CommandFailure::Unsupported, 0, got.detail);
    const Json wl = jsonObj(got.value, "WhiteLed");
    Json set = Json{{"channel", channel}, {"state", jsonInt(wl, "state", 0) ? 0 : 1}};
    for (const char *k : {"mode", "bright", "LightingSchedule"})
        if (wl.contains(k))
            set[k] = jsonRef(wl, k); // preserve settable fields verbatim
    const api::CommandResult put =
        http.callOne(QStringLiteral("SetWhiteLed"), Json{{"WhiteLed", set}}, /*action=*/0);
    return put.ok ? success() : failure(CommandFailure::Rejected, 0, put.detail);
}

} // namespace rl::device
