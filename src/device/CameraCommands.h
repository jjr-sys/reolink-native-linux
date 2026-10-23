#pragma once

#include "protocol/BaichuanLink.h"
#include "protocol/ReolinkTransport.h"

#include <QByteArray>
#include <QString>
#include <QVariantMap>

// What the app can ask a camera to do through its own protocol: alert-action toggles,
// the weekly recording schedule, generic Baichuan config reads and writes, the floodlight
// toggle. Each call is blocking (worker threads only), opens its own short Baichuan
// session through the injected opener, and reports a plain result; DeviceManager adds the
// row bookkeeping and the user-facing wording. The XML the device speaks is parsed here
// and nowhere else.
namespace rl::device {

enum class CommandFailure {
    None,
    Unreachable,   // the session could not be opened
    Rejected,      // the device answered with a failing status (see `status`)
    NotFound,      // the device does not have what was asked for
    Unsupported,   // the device does not support the command (see `detail`)
    UnknownKind,   // the caller named an alert kind that does not exist
};

struct CommandResult
{
    bool ok = false;
    CommandFailure failure = CommandFailure::None;
    int status = 0;     // Baichuan status code when Rejected
    QString detail;     // the device's own words, when it gave any
};

class CameraCommands
{
public:
    explicit CameraCommands(BaichuanOpener opener) : m_open(std::move(opener)) {}

    // ---- alert actions ("push" | "email" | "ftp") ----
    // Baichuan cmd_id pairs, verified on the RLN8-410. False for an unknown kind.
    static bool alertCommandIds(const QString &kind, quint32 &getCmd, quint32 &setCmd);
    // { ok, push, email, ftp }: each enable is 0/1, or -1 when the device did not say.
    // ok is false when the session could not be opened or no enable could be read.
    QVariantMap fetchAlerts(const BaichuanParams &host, int channel) const;
    CommandResult setAlert(const BaichuanParams &host, int channel, const QString &kind,
                           bool enable) const;

    // ---- weekly recording schedule (cmd 81/82) ----
    // { enable, <type>: 168-char '0'/'1' table } for each type the device lists
    // (Normal, MD, people, vehicle, dog_cat). Empty when it could not be read.
    static QVariantMap parseRecSchedule(const QByteArray &xml);
    QVariantMap fetchRecSchedule(const BaichuanParams &host, int channel) const;
    // Replace one type's table, leaving every other byte of the device's XML as it was.
    CommandResult writeRecSchedule(const BaichuanParams &host, int channel, const QString &type,
                                   const QString &table) const;

    // ---- generic Baichuan config ----
    QVariantMap fetchConfig(const BaichuanParams &host, int channel, quint32 cmdId,
                            const QByteArray &reqBody) const;
    CommandResult writeConfig(const BaichuanParams &host, int channel, quint32 getCmd,
                              quint32 setCmd, const QVariantMap &changes,
                              const QByteArray &reqBody) const;

    // ---- HTTP: floodlight ----
    // Flip the white LED on<->off, preserving brightness and the auto/schedule mode.
    // `Unsupported` carries the device's reason in `detail` when it refused the read;
    // `Rejected` when it refused the write.
    static CommandResult toggleFloodlight(ReolinkTransport &http, int channel);

private:
    BaichuanOpener m_open;
};

} // namespace rl::device
