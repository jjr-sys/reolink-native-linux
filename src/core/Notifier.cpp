#include "Notifier.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantList>
#include <QVariantMap>

namespace rl {

static constexpr auto kService = "org.freedesktop.Notifications";
static constexpr auto kPath = "/org/freedesktop/Notifications";
static constexpr auto kInterface = "org.freedesktop.Notifications";

Notifier::Notifier(QObject *parent) : QObject(parent)
{
    // Click ("default" action) + close signals from the notification daemon.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                    QStringLiteral("ActionInvoked"), this,
                    SLOT(onActionInvoked(quint32, QString)));
        bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                    QStringLiteral("ActivationToken"), this,
                    SLOT(onActivationToken(quint32, QString)));
        bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
                    QStringLiteral("NotificationClosed"), this,
                    SLOT(onNotificationClosed(quint32, quint32)));
    }
}

void Notifier::notify(const QString &title, const QString &body, const QString &iconName,
                      qint64 hostId, int channel, qint64 timestamp)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;

    // org.freedesktop.Notifications.Notify(app_name, replaces_id, app_icon,
    //   summary, body, actions, hints, expire_timeout)
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
        QStringLiteral("Notify"));
    QStringList actions;
    if (hostId >= 0) // "default" = clicking the notification body itself
        actions << QStringLiteral("default") << tr("View recording");
    msg << QStringLiteral("Reolink Client")   // app_name
        << quint32(0)                          // replaces_id (0 = new bubble)
        << iconName                            // app_icon
        << title                               // summary
        << body                                // body
        << actions                             // actions
        << QVariantMap{                        // hints
               {QStringLiteral("desktop-entry"),
                QStringLiteral("io.github.todesengelx.ReolinkLinux")},
               {QStringLiteral("x-kde-xdgTokenAppId"),
                QStringLiteral("io.github.todesengelx.ReolinkLinux")},
           }
        << qint32(7000);                       // expire timeout (ms)

    if (hostId < 0) {
        bus.asyncCall(msg); // fire-and-forget; never blocks the GUI thread
        return;
    }
    // Track the daemon-assigned id so a later click maps back to this event.
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, hostId, channel, timestamp](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<quint32> reply = *w;
                w->deleteLater();
                if (!reply.isError())
                    m_pending.insert(reply.value(), {hostId, channel, timestamp, QString()});
            });
}

void Notifier::alert(const QString &key, const QString &title, const QString &body,
                     bool critical, qint64 hostId, int channel)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;

    QVariantMap hints{
        {QStringLiteral("desktop-entry"), QStringLiteral("io.github.todesengelx.ReolinkLinux")},
        {QStringLiteral("x-kde-xdgTokenAppId"),
         QStringLiteral("io.github.todesengelx.ReolinkLinux")},
        // The spec types urgency as a byte: 1 normal, 2 critical (stays until dismissed).
        {QStringLiteral("urgency"), QVariant::fromValue(uchar(critical ? 2 : 1))},
    };
    if (critical)   // freedesktop sound theme; played once by the notification daemon
        hints.insert(QStringLiteral("sound-name"), QStringLiteral("alarm-clock-elapsed"));

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
        QStringLiteral("Notify"));
    msg << QStringLiteral("Reolink Client")
        << m_alertIds.value(key, 0)                // replaces the key's earlier bubble
        << QStringLiteral("io.github.todesengelx.ReolinkLinux")
        << title << body
        << QStringList{QStringLiteral("default"), tr("Open Live View")}
        << hints
        << qint32(critical ? 0 : 7000);           // 0 = never expires

    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, key, hostId, channel](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<quint32> reply = *w;
                w->deleteLater();
                if (reply.isError())
                    return;
                m_alertIds.insert(key, reply.value());
                m_pending.insert(reply.value(), {hostId, channel, 0, QString(), true});
            });
}

void Notifier::onActivationToken(quint32 id, const QString &token)
{
    const auto it = m_pending.find(id);
    if (it != m_pending.end())
        it->token = token;
}

void Notifier::onActionInvoked(quint32 id, const QString &actionKey)
{
    Q_UNUSED(actionKey);
    const auto it = m_pending.constFind(id);
    if (it == m_pending.constEnd())
        return; // not ours (the signal is bus-wide for all apps' notifications)
    if (!it->token.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", it->token.toUtf8());
    if (it->live)
        emit liveActivated(it->hostId, it->channel);
    else
        emit activated(it->hostId, it->channel, it->timestamp);
}

void Notifier::onNotificationClosed(quint32 id, quint32 reason)
{
    Q_UNUSED(reason);
    m_pending.remove(id);
    for (auto it = m_alertIds.begin(); it != m_alertIds.end(); ++it) {
        if (it.value() == id) {   // dismissed: the key's next alert is a fresh bubble
            m_alertIds.erase(it);
            break;
        }
    }
}

} // namespace rl
