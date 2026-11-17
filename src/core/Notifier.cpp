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
static constexpr auto kAppId = "io.github.todesengelx.ReolinkLinux";

// The hints every notice carries: the app's desktop entry (icon, name, focus on click).
static QVariantMap appHints()
{
    return {{QStringLiteral("desktop-entry"), QString::fromLatin1(kAppId)},
            {QStringLiteral("x-kde-xdgTokenAppId"), QString::fromLatin1(kAppId)}};
}

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
        << appHints()                          // hints
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

    // One request per key at a time: a later alert waits for the earlier bubble's id, so it
    // replaces that bubble instead of leaving it behind.
    if (m_alertSending.contains(key)) {
        m_alertQueued.insert(key, {title, body, critical, hostId, channel});
        return;
    }
    QVariantMap hints = appHints();
    // The spec types urgency as a byte: 1 normal, 2 critical (stays until dismissed).
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue(uchar(critical ? 2 : 1)));
    if (critical)   // freedesktop sound theme; played once by the notification daemon
        hints.insert(QStringLiteral("sound-name"), QStringLiteral("alarm-clock-elapsed"));

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kInterface),
        QStringLiteral("Notify"));
    msg << QStringLiteral("Reolink Client")
        << m_alertIds.value(key, 0)                // replaces the key's earlier bubble
        << QString::fromLatin1(kAppId)
        << title << body
        << QStringList{QStringLiteral("default"), tr("Open Live View")}
        << hints
        << qint32(critical ? 0 : 7000);           // 0 = never expires

    m_alertSending.insert(key);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, key, hostId, channel](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<quint32> reply = *w;
                w->deleteLater();
                m_alertSending.remove(key);
                if (!reply.isError()) {
                    m_alertIds.insert(key, reply.value());
                    m_pending.insert(reply.value(), {hostId, channel, 0, QString(), true});
                }
                if (m_alertQueued.contains(key)) {
                    const QueuedAlert q = m_alertQueued.take(key);
                    alert(key, q.title, q.body, q.critical, q.hostId, q.channel);
                }
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
