#include "device/DeviceSettings.h"

#include "device/QuickReply.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

namespace rl {

namespace {

QString notReady() { return DeviceSettings::tr("device not ready"); }
QString needsAdmin() { return DeviceSettings::tr("requires an administrator account"); }
QString unreachable() { return DeviceSettings::tr("couldn't reach the device"); }

// An HTTP answer that says it worked: code 0 and, when the value carries one, rspCode 200
// (reolink_aio).
bool httpOk(const api::CommandResult &r)
{
    return r.ok && (!r.value.is_object() || !r.value.contains("rspCode")
                    || r.value.value("rspCode", 200) == 200);
}

// The doorbell's clips: HTTP first; an empty or refused HTTP list is cross-checked over
// Baichuan, since an NVR can answer the HTTP list with nothing while the device has clips.
// `error` is set only when neither path could be read at all.
quickreply::ClipList fetchClipList(ReolinkTransport &http, const BaichuanOpener &opener,
                                   const BaichuanParams &p, int ch, QString *error)
{
    namespace qr = quickreply;
    qr::ClipList list;
    const api::CommandResult r =
        http.callOne(QStringLiteral("GetAudioFileList"), Json{{"channel", ch}}, 0);
    if (r.ok)
        list = qr::parseHttpList(r.value);
    if (!list.clips.isEmpty())
        return list;
    const auto bc = opener(p);
    if (bc && bc->open()) {
        quint16 st = 0;
        const QByteArray xml = bc->get(qr::kBcListCmd, ch, &st);
        bc->close();
        if (qr::baichuanStatusOk(st))
            list = qr::parseBaichuanList(xml);
        else if (!r.ok && error)
            *error = DeviceSettings::tr("the device does not support quick replies (status %1)").arg(st);
    } else if (!r.ok && error) {
        *error = r.detail.isEmpty() ? DeviceSettings::tr("could not reach the device") : r.detail;
    }
    return list;
}

QVariantList clipsToVariant(const quickreply::ClipList &list)
{
    QVariantList clips;
    for (const quickreply::Clip &c : list.clips)
        clips.append(QVariantMap{{QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name}});
    return clips;
}

} // namespace

DeviceSettings::DeviceSettings(device::CameraLookup lookup, BaichuanOpener opener, QObject *parent)
    : QObject(parent), m_lookup(std::move(lookup)), m_opener(opener), m_commands(opener)
{
}

DeviceSettings::~DeviceSettings()
{
    m_pending.waitForFinished();
}

QString DeviceSettings::refusal(const device::CameraLink &cam, bool write)
{
    if (!cam.ready || !cam.http)
        return notReady();
    if (write && !cam.admin)
        return needsAdmin();
    return {};
}

template <typename Work, typename Done>
void DeviceSettings::run(Work work, Done done)
{
    m_pending.addFuture(QtConcurrent::run([this, work, done] {
        auto result = work();
        QMetaObject::invokeMethod(this, [done, result] { done(result); }, Qt::QueuedConnection);
    }));
}

// ---- HTTP settings ------------------------------------------------------------------------

void DeviceSettings::fetchSettings(const QString &key, const QStringList &getCommands)
{
    const device::CameraLink cam = m_lookup(key);
    const QString why = getCommands.isEmpty() ? notReady() : refusal(cam, false);
    if (!why.isEmpty()) {
        emit settingsFailed(key, why);
        return;
    }
    const auto http = cam.http;
    const int ch = cam.channel;
    struct Fetched {
        QVariantMap values;
        QStringList failed;
        QString lastError;
    };
    run([http, ch, getCommands] {
        Fetched out;
        // One command, retrying only transient transport failures: NVRs (e.g. RLN8-410)
        // intermittently answer HTTP 502 when proxying a camera command under load. A few
        // tries with a short backoff (the lighter action=0 on the 2nd); a definitive device
        // error (rspCode set, e.g. -9 "not supported") stops at once.
        auto fetchOne = [&](const QString &cmd) {
            api::CommandResult r;
            for (int attempt = 0; attempt < 3; ++attempt) {
                r = http->callOne(cmd, Json{{"channel", ch}}, attempt == 1 ? 0 : 1);
                if (r.ok || r.rspCode != 0)
                    break;
                QThread::msleep(400);
            }
            return r;
        };
        // Newer firmware renamed several commands with a "V20" suffix (another schedule
        // shape). When the device doesn't know the legacy name, ask for the V20 one and keep
        // it under the legacy key so panels bind the same way.
        static const QHash<QString, QString> v20Alt = {
            {QStringLiteral("GetPush"), QStringLiteral("GetPushV20")},
            {QStringLiteral("GetEmail"), QStringLiteral("GetEmailV20")},
            {QStringLiteral("GetFtp"), QStringLiteral("GetFtpV20")},
            {QStringLiteral("GetRec"), QStringLiteral("GetRecV20")}};
        for (const QString &c : getCommands) {
            api::CommandResult r = fetchOne(c);
            if (!r.ok && r.rspCode != 0 && v20Alt.contains(c))
                r = fetchOne(v20Alt.value(c));
            if (r.ok) {
                out.values.insert(c, api::toVariant(r.value));
            } else {
                out.failed.append(c);
                if (!r.detail.isEmpty())
                    out.lastError = r.detail;
            }
        }
        return out;
    }, [this, key](const Fetched &f) {
        if (f.values.isEmpty()) {
            emit settingsFailed(key, f.lastError.isEmpty() ? tr("settings fetch failed") : f.lastError);
            return;
        }
        // Which commands the device refused, so panels show "unavailable" honestly
        // instead of misleading defaults.
        QVariantMap v = f.values;
        if (!f.failed.isEmpty())
            v.insert(QStringLiteral("_failed"), f.failed);
        emit settingsLoaded(key, v);
    });
}

void DeviceSettings::applySetting(const QString &key, const QString &setCommand,
                                  const QVariantMap &param)
{
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, true); !why.isEmpty()) {
        emit settingApplied(key, setCommand, false, why);
        return;
    }
    const auto http = cam.http;
    const Json p = api::toJson(param);
    run([http, setCommand, p] { return http->callOne(setCommand, p, /*action=*/0); },
        [this, key, setCommand](const api::CommandResult &r) {
            emit settingApplied(key, setCommand, r.ok,
                                r.ok ? QString() : (r.detail.isEmpty() ? tr("failed") : r.detail));
        });
}

void DeviceSettings::reboot(const QString &key)
{
    applySetting(key, QStringLiteral("Reboot"), {});
}

void DeviceSettings::toggleFloodlight(const QString &key)
{
    const QString cmd = QStringLiteral("SetWhiteLed");
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, true); !why.isEmpty()) {
        emit settingApplied(key, cmd, false, why);
        return;
    }
    const auto http = cam.http;
    const int ch = cam.channel;
    run([http, ch] { return device::CameraCommands::toggleFloodlight(*http, ch); },
        [this, key, cmd](const device::CommandResult &r) {
            QString error;
            if (!r.ok) {
                const bool unsupported = r.failure == device::CommandFailure::Unsupported;
                error = !r.detail.isEmpty() ? r.detail
                        : unsupported       ? tr("floodlight not supported")
                                            : tr("failed");
            }
            emit settingApplied(key, cmd, r.ok, error);
        });
}

// ---- Doorbell quick replies ---------------------------------------------------------------

void DeviceSettings::fetchQuickReplies(const QString &key)
{
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, false); !why.isEmpty()) {
        emit quickRepliesLoaded(key, {}, 0, why);
        return;
    }
    const auto http = cam.http;
    const BaichuanOpener opener = m_opener;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    struct Clips {
        quickreply::ClipList list;
        QString error;
    };
    run([http, opener, p, ch] {
        Clips c;
        c.list = fetchClipList(*http, opener, p, ch, &c.error);
        return c;
    }, [this, key](const Clips &c) {
        emit quickRepliesLoaded(key, clipsToVariant(c.list), c.list.maxFiles, c.error);
    });
}

void DeviceSettings::playQuickReply(const QString &key, int clipId)
{
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, false); !why.isEmpty()) {
        emit quickReplyPlayed(key, clipId, false, why);
        return;
    }
    const auto http = cam.http;
    const BaichuanOpener opener = m_opener;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    run([http, opener, p, ch, clipId] {
        namespace qr = quickreply;
        std::pair<bool, QString> out{false, {}};
        const api::CommandResult r = http->callOne(
            QStringLiteral("QuickReplyPlay"), Json{{"id", clipId}, {"channel", ch}}, 0);
        out.first = httpOk(r);
        if (!out.first && (r.ok || qr::httpShouldFallBack(r.rspCode) || r.rspCode == 0)) {
            const auto bc = opener(p);
            if (bc && bc->open()) {
                quint16 st = 0;
                bc->transact(qr::kBcPlayCmd, ch, qr::baichuanPlayBody(ch, clipId), &st);
                bc->close();
                out.first = qr::baichuanStatusOk(st);
                if (!out.first)
                    out.second = tr("the doorbell refused the clip (status %1)").arg(st);
            } else {
                out.second = tr("could not reach the device");
            }
        } else if (!out.first) {
            out.second = r.detail.isEmpty() ? tr("failed") : r.detail;
        }
        return out;
    }, [this, key, clipId](const std::pair<bool, QString> &r) {
        emit quickReplyPlayed(key, clipId, r.first, r.second);
    });
}

// Auto-reply: the current setting plus the clips it can pick from. The setting is read over
// HTTP, then Baichuan cmd 427 when HTTP doesn't answer.
void DeviceSettings::fetchAutoReply(const QString &key)
{
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, false); !why.isEmpty()) {
        emit autoReplyLoaded(key, {}, {}, why);
        return;
    }
    const auto http = cam.http;
    const BaichuanOpener opener = m_opener;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    struct Loaded {
        QVariantMap setting;
        QVariantList clips;
        QString error;
    };
    run([http, opener, p, ch] {
        namespace qr = quickreply;
        Loaded out;
        qr::AutoReply a;
        const api::CommandResult r =
            http->callOne(QStringLiteral("GetAutoReply"), Json{{"channel", ch}}, 0);
        if (r.ok)
            a = qr::parseHttpAutoReply(r.value);
        if (!a.valid) {
            const auto bc = opener(p);
            if (bc && bc->open()) {
                quint16 st = 0;
                const QByteArray xml = bc->get(qr::kBcAutoReplyGetCmd, ch, &st);
                bc->close();
                if (qr::baichuanStatusOk(st))
                    a = qr::parseBaichuanAutoReply(xml);
            }
        }
        if (a.valid) {
            QString clipError;
            out.clips = clipsToVariant(fetchClipList(*http, opener, p, ch, &clipError));
        } else {
            out.error = tr("this device does not offer auto-reply");
        }
        out.setting = {{QStringLiteral("enable"), a.enable},
                       {QStringLiteral("fileId"), a.fileId},
                       {QStringLiteral("timeout"), a.timeout}};
        return out;
    }, [this, key](const Loaded &l) { emit autoReplyLoaded(key, l.setting, l.clips, l.error); });
}

void DeviceSettings::setAutoReply(const QString &key, bool enable, int fileId, int timeout)
{
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, true); !why.isEmpty()) {
        emit autoReplySaved(key, false, why);
        return;
    }
    const auto http = cam.http;
    const BaichuanOpener opener = m_opener;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    run([http, opener, p, ch, enable, fileId, timeout] {
        namespace qr = quickreply;
        qr::AutoReply a;
        a.valid = true;
        a.enable = enable;
        a.fileId = fileId;
        a.timeout = timeout;
        const api::CommandResult r =
            http->callOne(QStringLiteral("SetAutoReply"), qr::httpAutoReplyParam(ch, a), 0);
        std::pair<bool, QString> out{httpOk(r), {}};
        if (!out.first) {
            // As the clip list: try the native protocol.
            const auto bc = opener(p);
            if (bc && bc->open()) {
                out.first = bc->writeFields(qr::kBcAutoReplyGetCmd, qr::kBcAutoReplySetCmd, ch,
                                            qr::baichuanAutoReplyChanges(a), {});
                bc->close();
            }
            if (!out.first)
                out.second = r.detail.isEmpty() ? tr("the device refused the change") : r.detail;
        }
        return out;
    }, [this, key](const std::pair<bool, QString> &r) { emit autoReplySaved(key, r.first, r.second); });
}

// ---- Baichuan -----------------------------------------------------------------------------

void DeviceSettings::fetchAlerts(const QString &key)
{
    const device::CameraLink cam = m_lookup(key);
    if (!cam.ready) {
        emit alertsLoaded(key, {});
        return;
    }
    const device::CameraCommands commands = m_commands;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    run([commands, p, ch] { return commands.fetchAlerts(p, ch); },
        [this, key](const QVariantMap &m) {
            if (m.contains(QStringLiteral("push")))
                m_pushEnabled[key] = m.value(QStringLiteral("push")).toInt();
            emit alertsLoaded(key, m);
        });
}

void DeviceSettings::setAlertEnable(const QString &key, const QString &kind, bool enable)
{
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, true); !why.isEmpty()) {
        emit settingApplied(key, kind, false, why);
        return;
    }
    quint32 getCmd = 0, setCmd = 0;
    if (!device::CameraCommands::alertCommandIds(kind, getCmd, setCmd)) {
        emit settingApplied(key, kind, false, tr("unknown alert \"%1\"").arg(kind));
        return;
    }
    const device::CameraCommands commands = m_commands;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    run([commands, p, ch, kind, enable] { return commands.setAlert(p, ch, kind, enable).ok; },
        [this, key, kind, enable](bool ok) {
            if (ok && kind == QLatin1String("push"))
                m_pushEnabled[key] = enable ? 1 : 0;
            emit settingApplied(key, kind, ok, ok ? QString() : unreachable());
        });
}

void DeviceSettings::fetchRecSchedule(const QString &key)
{
    const device::CameraLink cam = m_lookup(key);
    if (!cam.ready) {
        emit recScheduleLoaded(key, {});
        return;
    }
    const device::CameraCommands commands = m_commands;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    run([commands, p, ch] { return commands.fetchRecSchedule(p, ch); },
        [this, key](const QVariantMap &m) { emit recScheduleLoaded(key, m); });
}

void DeviceSettings::writeRecSchedule(const QString &key, const QString &type, const QString &table)
{
    const QString cmd = QStringLiteral("SetRecSchedule");
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, true); !why.isEmpty()) {
        emit settingApplied(key, cmd, false, why);
        return;
    }
    const device::CameraCommands commands = m_commands;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    run([commands, p, ch, type, table] { return commands.writeRecSchedule(p, ch, type, table); },
        [this, key, cmd](const device::CommandResult &r) {
            QString err;
            switch (r.failure) {
            case device::CommandFailure::None: break;
            case device::CommandFailure::Unreachable: err = unreachable(); break;
            case device::CommandFailure::NotFound: err = tr("schedule type not found on this device"); break;
            default: err = tr("device rejected the schedule (status %1)").arg(r.status); break;
            }
            emit settingApplied(key, cmd, r.ok, err);
        });
}

void DeviceSettings::fetchBcConfig(const QString &key, int cmdId, const QString &reqBody)
{
    const device::CameraLink cam = m_lookup(key);
    if (!cam.ready) {
        emit bcConfigLoaded(key, cmdId, {});
        return;
    }
    const device::CameraCommands commands = m_commands;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    const QByteArray body = reqBody.toUtf8();
    run([commands, p, ch, cmdId, body] {
        return commands.fetchConfig(p, ch, static_cast<quint32>(cmdId), body);
    }, [this, key, cmdId](const QVariantMap &m) { emit bcConfigLoaded(key, cmdId, m); });
}

void DeviceSettings::writeBcConfig(const QString &key, int getCmd, int setCmd,
                                   const QVariantMap &changes, const QString &reqBody)
{
    const QString label = QStringLiteral("Set%1").arg(setCmd);
    const device::CameraLink cam = m_lookup(key);
    if (const QString why = refusal(cam, true); !why.isEmpty()) {
        emit settingApplied(key, label, false, why);
        return;
    }
    const device::CameraCommands commands = m_commands;
    const BaichuanParams p = cam.baichuan;
    const int ch = cam.channel;
    const QByteArray body = reqBody.toUtf8();
    run([commands, p, ch, getCmd, setCmd, changes, body] {
        return commands.writeConfig(p, ch, static_cast<quint32>(getCmd),
                                    static_cast<quint32>(setCmd), changes, body).ok;
    }, [this, key, label](bool ok) {
        emit settingApplied(key, label, ok, ok ? QString() : unreachable());
    });
}

// ---- Motion zone grid ---------------------------------------------------------------------

QString DeviceSettings::mdZoneBits(const QString &valueTable, int cells) const
{
    const QByteArray raw = QByteArray::fromBase64(valueTable.toLatin1());
    QString out;
    out.reserve(cells);
    for (int i = 0; i < cells; ++i) {
        const int byte = i / 8;
        const bool on = byte < raw.size()
                        && ((static_cast<quint8>(raw[byte]) >> (7 - (i % 8))) & 1);
        out += on ? QLatin1Char('1') : QLatin1Char('0');
    }
    return out;
}

QString DeviceSettings::mdZoneTable(const QString &bits) const
{
    const int cells = int(bits.size());
    QByteArray raw((cells + 7) / 8, '\0');
    for (int i = 0; i < cells; ++i)
        if (bits.at(i) == QLatin1Char('1'))
            raw[i / 8] = static_cast<char>(static_cast<quint8>(raw[i / 8]) | (1 << (7 - (i % 8))));
    return QString::fromLatin1(raw.toBase64());
}

// ---- The push switch ----------------------------------------------------------------------

bool DeviceSettings::pushEnabledFor(const QString &key) const
{
    return m_pushEnabled.value(key, 1) != 0;
}

void DeviceSettings::warmPushCache(const QStringList &keys)
{
    // Staggered, so several Baichuan settings sessions never open at once.
    int i = 0;
    for (const QString &key : keys)
        QTimer::singleShot(i++ * 1200, this, [this, key] { fetchAlerts(key); });
}

} // namespace rl
