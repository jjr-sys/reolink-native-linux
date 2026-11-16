#pragma once

#include "device/CameraCommands.h"
#include "protocol/BaichuanLink.h"
#include "protocol/ReolinkTransport.h"

#include <QFutureSynchronizer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace rl {

namespace device {

// What the settings commands need to ask one camera, as DeviceManager knows it.
struct CameraLink {
    bool ready = false;   // a Reolink login is loaded: the camera can be asked
    bool admin = false;   // the login may change settings
    int channel = 0;
    std::shared_ptr<ReolinkTransport> http;
    BaichuanParams baichuan;
};
// The camera for a key ("hostId:channel"); not ready when unknown.
using CameraLookup = std::function<CameraLink(const QString &cameraKey)>;

} // namespace device

// A camera's settings, read and written on its behalf (wayfinder/architecture ticket 08):
// the HTTP settings commands, quick replies and auto-reply, alert switches, the recording
// schedule, Baichuan config, the floodlight and reboot. Every call names the camera by its
// key and answers by signal with the same key, so an answer can't land on another camera
// when the list's rows shift. Calls run on worker threads; going waits for them. The
// device protocol is CameraCommands' and quickreply's; this adds the camera lookup, the
// administrator check and the wording. A second command set (Bosch) would sit beside the
// Reolink one, picked by the camera's kind.
class DeviceSettings : public QObject
{
    Q_OBJECT

public:
    DeviceSettings(device::CameraLookup lookup, BaichuanOpener opener, QObject *parent = nullptr);
    ~DeviceSettings() override;

    // ---- HTTP settings ----
    // The Get* commands' answers by name (a refused one listed under "_failed"); a legacy
    // name the device doesn't know is retried as its V20 name and answered under the old.
    Q_INVOKABLE void fetchSettings(const QString &key, const QStringList &getCommands);
    Q_INVOKABLE void applySetting(const QString &key, const QString &setCommand,
                                  const QVariantMap &param);
    Q_INVOKABLE void reboot(const QString &key);
    // Flip the white LED (answers settingApplied "SetWhiteLed").
    Q_INVOKABLE void toggleFloodlight(const QString &key);

    // ---- Doorbell quick replies ----
    Q_INVOKABLE void fetchQuickReplies(const QString &key);
    Q_INVOKABLE void playQuickReply(const QString &key, int clipId);
    Q_INVOKABLE void fetchAutoReply(const QString &key);
    Q_INVOKABLE void setAutoReply(const QString &key, bool enable, int fileId, int timeout);

    // ---- Baichuan ----
    // Alert switches ("push" | "email" | "ftp"); a write answers settingApplied(kind).
    Q_INVOKABLE void fetchAlerts(const QString &key);
    Q_INVOKABLE void setAlertEnable(const QString &key, const QString &kind, bool enable);
    // The weekly recording schedule: { enable, <type>: 168 '0'/'1' }; a write answers
    // settingApplied "SetRecSchedule".
    Q_INVOKABLE void fetchRecSchedule(const QString &key);
    Q_INVOKABLE void writeRecSchedule(const QString &key, const QString &type, const QString &table);
    // Generic config by Baichuan command id; a write answers settingApplied "Set<setCmd>".
    Q_INVOKABLE void fetchBcConfig(const QString &key, int cmdId, const QString &reqBody = {});
    Q_INVOKABLE void writeBcConfig(const QString &key, int getCmd, int setCmd,
                                   const QVariantMap &changes, const QString &reqBody = {});

    // ---- Motion zone grid (pure) ----
    // The device's base64 zone table as '0'/'1' per cell, row by row, and back.
    Q_INVOKABLE QString mdZoneBits(const QString &valueTable, int cells) const;
    Q_INVOKABLE QString mdZoneTable(const QString &bits) const;

    // ---- The push switch, for desktop notices ----
    // Whether the camera's push alert is on, as last read or written. Unknown (never read,
    // or not reported) allows a notice; only an explicit off stops it.
    bool pushEnabledFor(const QString &key) const;
    // Read each camera's alert switches once, staggered (one Baichuan session at a time).
    void warmPushCache(const QStringList &keys);

signals:
    void settingsLoaded(const QString &key, const QVariantMap &values);
    void settingsFailed(const QString &key, const QString &error);
    void settingApplied(const QString &key, const QString &command, bool ok, const QString &error);
    void quickRepliesLoaded(const QString &key, const QVariantList &clips, int maxFiles,
                            const QString &error);
    void quickReplyPlayed(const QString &key, int clipId, bool ok, const QString &error);
    void autoReplyLoaded(const QString &key, const QVariantMap &setting, const QVariantList &clips,
                         const QString &error);
    void autoReplySaved(const QString &key, bool ok, const QString &error);
    void alertsLoaded(const QString &key, const QVariantMap &values);
    void recScheduleLoaded(const QString &key, const QVariantMap &values);
    void bcConfigLoaded(const QString &key, int cmdId, const QVariantMap &values);

private:
    // Runs `work` on a worker thread and `done` with its result back on this object's thread.
    template <typename Work, typename Done>
    void run(Work work, Done done);

    device::CameraLookup m_lookup;
    BaichuanOpener m_opener;
    device::CameraCommands m_commands;
    QHash<QString, int> m_pushEnabled;   // camera key -> push switch (1/0/-1)
    QFutureSynchronizer<void> m_pending;
};

} // namespace rl
