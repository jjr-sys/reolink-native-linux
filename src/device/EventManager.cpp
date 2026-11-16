#include "EventManager.h"

#include "DeviceManager.h"
#include "device/EventHistory.h"
#include "device/EventSnapshot.h"
#include "device/FrigateApi.h"

#include "core/Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <utility>

namespace rl {


EventManager::EventManager(Database *db, DeviceManager *devices, QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_devices(devices),
      m_inbox(db, devices->eventSwitches())
{
    connect(&m_inbox, &Inbox::trimmed, this, [this] { m_trimmed = true; });
    m_desktopNotifications =
        QSettings().value(QStringLiteral("notifications/desktop"), true).toBool();
    normalizeStoredCameraNames();
    m_inbox.enforceRetention();
    m_trimmed = false;   // the list isn't loaded yet: the reload below is the first
    reload();

    // This launch's run, stamped every minute, so a gap (app closed or crashed) can be
    // told apart from a quiet period.
    m_runId = m_db->beginAppRun(QDateTime::currentSecsSinceEpoch());
    m_runTimer.setInterval(int(device::kRunHeartbeatSecs * 1000));
    connect(&m_runTimer, &QTimer::timeout, this,
            [this] { m_db->touchAppRun(m_runId, QDateTime::currentSecsSinceEpoch()); });
    m_runTimer.start();
    connect(m_devices, &DeviceManager::detectionEvent, this, &EventManager::onDetection);
    connect(m_devices->eventSwitches(), &EventSwitches::changed, this,
            &EventManager::switchesChanged);
    connect(&m_notifier, &Notifier::activated, this, &EventManager::eventActivated);
    connect(m_devices, &DeviceManager::eventThumbnailReady, this,
            &EventManager::onThumbnailReady);
    connect(m_devices, &DeviceManager::connectivityChanged, this,
            &EventManager::onConnectivity);

    // The age purge runs hourly; the cap is checked as events arrive.
    m_retentionTimer.setInterval(3600'000);
    connect(&m_retentionTimer, &QTimer::timeout, this, [this] {
        m_inbox.enforceRetention();
        reloadIfTrimmed();   // rows without thumbnails may have gone too
    });
    m_retentionTimer.start();

    // A Frigate host the inbox has never filed anything for gets one automatic
    // catch-up against its own event history — otherwise a quiet camera (or one
    // just added) looks empty in the app despite Frigate holding real clips,
    // until it happens to trigger a fresh live detection. Once it has any events
    // on record, this does not run again on later launches. Frigate cameras are
    // discovered asynchronously, so this re-checks on every device-list change
    // rather than running once here.
    connect(m_devices, &DeviceManager::countChanged, this, &EventManager::checkFrigateCatchup);
    checkFrigateCatchup();

    // Test hook: RL_MOCK_EVENTS seeds in-memory events (no DB writes) so the
    // inbox UI can be verified without cameras.
    if (qEnvironmentVariableIsSet("RL_MOCK_EVENTS")) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const struct {
            const char *type;
            const char *cam;
            qint64 ago;
        } mock[] = {
            {"person", "Front Door", 45},   {"vehicle", "Driveway", 600},
            {"pet", "Backyard", 3600},      {"visitor", "Front Door", 7200},
            {"motion", "Side Gate", 90000},
        };
        beginResetModel();
        m_rows.clear();
        for (const auto &m : mock) {
            EventRecord r;
            r.type = QString::fromUtf8(m.type);
            r.camera = QString::fromUtf8(m.cam);
            r.timestamp = now - m.ago;
            m_rows.append(r);
        }
        endResetModel();
        emit countChanged();
    }
}

EventManager::~EventManager()
{
    m_db->touchAppRun(m_runId, QDateTime::currentSecsSinceEpoch());
    m_pending.waitForFinished();
}

// Rewrite any Frigate event still stored under its raw config key ("bosch_camera")
// to the pretty form ("Bosch Camera") shown in the filter dropdown and the row —
// only events raised before the display-name fix shipped can still be raw. Safe to
// run every startup: rows already in the pretty form are left untouched.
void EventManager::normalizeStoredCameraNames()
{
    for (const QVariant &v : m_devices->hostIds()) {
        const qint64 hostId = v.toLongLong();
        if (!m_devices->hostInfo(hostId).value("frigateApi").toBool())
            continue;
        for (const QString &camera : m_db->eventCameras(hostId)) {
            const QString pretty = frigate::displayCameraName(camera);
            if (pretty != camera)
                m_db->renameEventCamera(hostId, camera, pretty);
        }
    }
}

void EventManager::reload()
{
    beginResetModel();
    m_rows = m_db->recentEvents(kListLimit);
    endResetModel();
    emit countChanged();
    emit storedChanged(-1);
}

int EventManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant EventManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const EventRecord &e = m_rows.at(index.row());
    switch (role) {
    case TypeRole:
        return e.type;
    case CameraRole:
        return e.camera;
    case TimestampRole:
        return e.timestamp;
    case TimeTextRole: {
        const QDateTime dt = QDateTime::fromSecsSinceEpoch(e.timestamp);
        const qint64 ago = QDateTime::currentSecsSinceEpoch() - e.timestamp;
        if (ago < 60)
            return tr("just now");
        if (ago < 3600)
            return tr("%1 min ago").arg(ago / 60);
        if (dt.date() == QDate::currentDate())
            return dt.toString(QStringLiteral("hh:mm"));
        return dt.toString(QStringLiteral("MMM d, hh:mm"));
    }
    case ThumbnailRole:
        return e.thumbnail;
    case HostIdRole:
        return e.hostId;
    case ChannelRole:
        return e.channel;
    case PropertyRole:
        return m_devices->hostName(e.hostId);
    }
    return {};
}

QHash<int, QByteArray> EventManager::roleNames() const
{
    return {
        {TypeRole, "type"},         {CameraRole, "camera"}, {TimestampRole, "timestamp"},
        {TimeTextRole, "timeText"}, {ThumbnailRole, "thumbnail"}, {HostIdRole, "hostId"},
        {ChannelRole, "channel"},   {PropertyRole, "property"},
    };
}

void EventManager::onDetection(qint64 hostId, int channel, const QString &type,
                               const QString &camera, qint64 at)
{
    // `at`: when the source saw it (Frigate's start: a poll after an outage reports late).
    const Inbox::Filing f = m_inbox.file({hostId, channel, type, camera, double(at)},
                                         Inbox::Source::Live);
    take(hostId, {f});
    // Grab a frame from the camera for the row's thumbnail (quiet; the row updates in
    // place when it lands): a new row, or the same event history filed without one —
    // unless Frigate's own snapshot for it is still on its way.
    if (f.needsThumbnail && !m_snapshotsDue.contains(f.row.id))
        m_devices->captureEventThumbnail(hostId, channel, f.row.id);
    if (!f.announce)
        return; // not stored, or already known: nothing to count or announce
    ++m_unread;
    emit unreadChanged();

    // Desktop notification for this detection, gated on the camera's Push
    // Notifications being enabled (mirrors how the official apps gate push).
    if (m_desktopNotifications && m_devices->settings()->pushEnabledFor(QStringLiteral("%1:%2").arg(hostId).arg(channel))) {
        QString what;
        if (type == QLatin1String("person"))       what = tr("Person detected");
        else if (type == QLatin1String("vehicle")) what = tr("Vehicle detected");
        else if (type == QLatin1String("pet") || type == QLatin1String("dog_cat"))
            what = tr("Pet detected");
        else if (type == QLatin1String("visitor")) what = tr("Visitor at the door");
        else                                        what = tr("Motion detected");
        m_notifier.notify(camera.isEmpty() ? tr("Camera") : camera, what,
                          QStringLiteral("io.github.todesengelx.ReolinkLinux"),
                          hostId, channel, f.row.timestamp);
    }
}

bool EventManager::take(qint64 hostId, const QVector<Inbox::Filing> &filed)
{
    bool stored = false;
    for (const Inbox::Filing &f : filed) {
        if (f.verdict != Inbox::Verdict::Stored)
            continue;
        stored = true;
        if (!m_trimmed)   // else the list reloads from the store below
            insertSorted(f.row);
    }
    if (stored)
        emit storedChanged(hostId);
    reloadIfTrimmed();
    return stored;
}

void EventManager::insertSorted(const EventRecord &row)
{
    // Newest first; a history row lands among the older ones.
    const auto at = std::find_if(m_rows.cbegin(), m_rows.cend(), [&row](const EventRecord &r) {
        return r.timestamp <= row.timestamp;
    });
    const int i = int(at - m_rows.cbegin());
    if (i >= kListLimit)
        return;
    beginInsertRows({}, i, i);
    m_rows.insert(i, row);
    endInsertRows();
    if (m_rows.size() > kListLimit) {
        beginRemoveRows({}, m_rows.size() - 1, m_rows.size() - 1);
        m_rows.removeLast();
        endRemoveRows();
    }
    emit countChanged();
}

void EventManager::reloadIfTrimmed()
{
    if (!m_trimmed)
        return;
    m_trimmed = false;
    reload();
}

void EventManager::setDesktopNotifications(bool on)
{
    if (m_desktopNotifications == on)
        return;
    m_desktopNotifications = on;
    QSettings().setValue(QStringLiteral("notifications/desktop"), on);
    emit desktopNotificationsChanged();
}

void EventManager::onConnectivity(qint64 hostId, int channel, const QString &name, bool online,
                                  const QVector<device::CameraRef> &changed)
{
    // Health alert: a camera (channel >= 0) or a whole host dropped or returned. A
    // host's change is filed as Status on each camera that flipped with it. Counted and
    // notified once — a dead camera can't announce itself, so the client has to.
    const QVector<Inbox::Filing> filed = m_inbox.fileStatus(hostId, channel, name, online, changed);
    take(hostId, filed);
    if (std::none_of(filed.cbegin(), filed.cend(), [](const Inbox::Filing &f) { return f.announce; }))
        return;
    ++m_unread;
    emit unreadChanged();

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (m_desktopNotifications)
        m_notifier.notify(name.isEmpty() ? tr("Device") : name,
                          online ? tr("Back online") : tr("Went offline \u26a0"),
                          QStringLiteral("io.github.todesengelx.ReolinkLinux"),
                          hostId, qMax(0, channel), now);
}

void EventManager::onThumbnailReady(qint64 eventId, const QString &path)
{
    if (!m_db->setEventThumbnail(eventId, path)) {
        QFile::remove(path); // event already trimmed away: don't keep an orphaned file
        return;
    }
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id != eventId)
            continue;
        m_rows[i].thumbnail = path;
        emit dataChanged(index(i), index(i), {ThumbnailRole});
        emit storedChanged(m_rows.at(i).hostId); // its camera's column fetches the path
        return;
    }
    emit storedChanged(-1); // an older event (a history sync): its recorder isn't at hand
}

QVariantList EventManager::eventTimesFor(qint64 hostId, int channel, qint64 dayStart) const
{
    QVariantList out;
    const qint64 dayEnd = device::dayContaining(dayStart, QTimeZone::systemTimeZone()).to;
    for (const EventRecord &e : m_db->eventsBetween(hostId, channel, dayStart, dayEnd)) {
        if (e.type == QLatin1String("offline") || e.type == QLatin1String("online"))
            continue; // connectivity events aren't recordings
        out.append(int(e.timestamp - dayStart));
    }
    return out;
}

QVariantList EventManager::notRunningSpans(qint64 dayStart) const
{
    const qint64 dayEnd = device::dayContaining(dayStart, QTimeZone::systemTimeZone()).to;
    QVariantList out;
    for (const device::TimeSpan &g : device::storedNotRunningGaps(
             *m_db, dayStart, dayEnd, QDateTime::currentSecsSinceEpoch()))
        out.append(QVariantMap{{"from", g.from}, {"to", g.to}});
    return out;
}

QVariantMap EventManager::dayCountsByHost(qint64 dayStart) const
{
    const qint64 dayEnd = device::dayContaining(dayStart, QTimeZone::systemTimeZone()).to;
    QVariantMap out;
    const auto counts = m_db->eventCountsByHost(dayStart, dayEnd);
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        QVariantMap byType;
        for (auto t = it.value().cbegin(); t != it.value().cend(); ++t)
            byType.insert(t.key(), t.value());
        out.insert(QString::number(it.key()), byType);
    }
    return out;
}

void EventManager::markAllRead()
{
    if (m_unread == 0)
        return;
    m_unread = 0;
    emit unreadChanged();
}

void EventManager::clear()
{
    // Drop the cached snapshot files along with the rows.
    QDir dir(Paths::thumbnailsDir());
    for (const QString &f : dir.entryList({QStringLiteral("event_*.jpg")}, QDir::Files))
        dir.remove(f);
    m_db->clearEvents();
    m_inbox.recount();
    m_unread = 0;
    emit unreadChanged();
    reload();
}

EventSwitches *EventManager::switches() const
{
    return m_devices->eventSwitches();
}

bool EventManager::isSwitchedOff(qint64 hostId, int channel, const QString &type) const
{
    return m_devices->eventSwitches()->isOff(hostId, channel, type);
}

QString EventManager::saveSnapshot(const QString &thumbnail, const QString &camera,
                                   qint64 timestamp) const
{
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pictures.isEmpty())
        pictures = QDir::homePath();
    const QString dir = pictures + QStringLiteral("/Reolink");
    if (thumbnail.isEmpty() || !QFile::exists(thumbnail) || !QDir().mkpath(dir))
        return {};
    QString name = camera.isEmpty() ? tr("Camera") : camera;
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    const QString base = dir + QLatin1Char('/') + name + QLatin1Char(' ')
                         + QDateTime::fromSecsSinceEpoch(timestamp).toString(
                             QStringLiteral("yyyy-MM-dd HH.mm.ss"));
    QString path = base + QStringLiteral(".jpg");
    for (int n = 2; QFile::exists(path); ++n)
        path = base + QStringLiteral(" (%1).jpg").arg(n);
    return QFile::copy(thumbnail, path) ? path : QString();
}

QVariantMap EventManager::switchStats(qint64 hostId, int channel, const QString &type) const
{
    QStringList thumbs;
    const int count = m_db->countEvents(hostId, channel, EventSwitches::storedType(type), &thumbs);
    qint64 bytes = 0;
    for (const QString &path : std::as_const(thumbs))
        bytes += QFileInfo(path).size(); // 0 for a file already gone
    return {{QStringLiteral("count"), count}, {QStringLiteral("bytes"), bytes}};
}

bool EventManager::switchOff(qint64 hostId, int channel, const QString &type)
{
    return switchOffAll({{hostId, channel, type}});
}

bool EventManager::switchOffAll(const QVector<EventSwitchKey> &pairs)
{
    // Each switch is recorded first: if it can't be saved, that pair's events stay. If a
    // delete fails the pair stays off (nothing new is stored) and the result is false.
    bool ok = true, deleted = false;
    QStringList thumbs;
    for (const EventSwitchKey &k : pairs) {
        if (!m_devices->eventSwitches()->setOff(k.hostId, k.channel, k.type, true)) {
            ok = false;
            continue;
        }
        QStringList removed;
        if (!m_db->deleteEvents(k.hostId, k.channel, EventSwitches::storedType(k.type), &removed)) {
            ok = false;
            continue;
        }
        thumbs += removed;
        m_inbox.recount(k.hostId);
        deleted = true;
    }
    Inbox::dropThumbnails(thumbs);
    if (deleted)
        reload();
    return ok;
}

bool EventManager::switchOn(qint64 hostId, int channel, const QString &type)
{
    return m_devices->eventSwitches()->setOff(hostId, channel, type, false);
}

namespace {
const char *kFirstSwitchOffsKey = "events/firstSwitchOffs";
const char *kFirstSwitchOffsAskedKey = "events/firstSwitchOffsAsked";
} // namespace

QVector<device::KnownCamera> EventManager::knownCameras() const
{
    QVector<device::KnownCamera> out;
    const auto stored = m_db->eventCameraNames();
    for (const HostRecord &h : m_db->hosts()) {
        if (device::capabilities(device::kindFromString(h.kind)).detections
            == device::DetectionSource::None)
            continue; // a bare stream raises nothing
        const QHash<int, QStringList> channelTypes = m_db->channelEventTypes(h.id);
        auto types = [&](int channel) {
            const QStringList known = channelTypes.value(channel);
            return known.isEmpty() ? m_devices->eventTypesFor(h.id, channel) : known;
        };
        for (auto it = stored.cbegin(); it != stored.cend(); ++it)
            if (it.key().first == h.id)
                out.append({h.id, h.name, it.key().second, it.value(), types(it.key().second)});
        // Channels with nothing stored yet: matched by "*" only, shown as "Channel N".
        for (auto c = channelTypes.cbegin(); c != channelTypes.cend(); ++c)
            if (!stored.contains({h.id, c.key()}))
                out.append({h.id, h.name, c.key(), tr("Channel %1").arg(c.key() + 1), c.value()});
    }
    return out;
}

QVariantMap EventManager::offerFirstSwitchOffs()
{
    m_offeredSwitchOffs.clear();
    QSettings settings;
    if (settings.value(QLatin1String(kFirstSwitchOffsAskedKey)).toBool())
        return {};
    const QStringList entries = settings.value(QLatin1String(kFirstSwitchOffsKey)).toStringList();
    if (entries.isEmpty())
        return {}; // no list: nothing to offer, and nothing settled either
    const QVector<device::KnownCamera> cameras = knownCameras();
    const QVector<EventSwitchKey> pairs = device::resolveFirstSwitchOffs(entries, cameras);
    if (pairs.isEmpty()) {
        declineFirstSwitchOffs(); // matches nothing: settled, so it never turns up by surprise
        return {};
    }
    QVariantList list;
    int count = 0;
    qint64 bytes = 0;
    for (const EventSwitchKey &k : pairs) {
        QString recorder, camera;
        for (const device::KnownCamera &c : cameras)
            if (c.hostId == k.hostId && c.channel == k.channel) {
                recorder = c.hostName;
                camera = c.camera;
                break;
            }
        const QVariantMap stats = switchStats(k.hostId, k.channel, k.type);
        const int n = stats.value(QStringLiteral("count")).toInt();
        count += n;
        bytes += stats.value(QStringLiteral("bytes")).toLongLong();
        list.append(QVariantMap{{QStringLiteral("hostId"), k.hostId},
                                {QStringLiteral("channel"), k.channel},
                                {QStringLiteral("type"), k.type},
                                {QStringLiteral("recorder"), recorder},
                                {QStringLiteral("camera"), camera},
                                {QStringLiteral("count"), n}});
    }
    m_offeredSwitchOffs = pairs;
    return {{QStringLiteral("pairs"), list},
            {QStringLiteral("count"), count},
            {QStringLiteral("bytes"), bytes}};
}

bool EventManager::confirmFirstSwitchOffs()
{
    if (m_offeredSwitchOffs.isEmpty())
        return false;
    const QVector<EventSwitchKey> pairs = std::exchange(m_offeredSwitchOffs, {});
    const bool ok = switchOffAll(pairs);
    declineFirstSwitchOffs(); // asked: not offered again
    return ok;
}

void EventManager::declineFirstSwitchOffs()
{
    m_offeredSwitchOffs.clear();
    QSettings().setValue(QLatin1String(kFirstSwitchOffsAskedKey), true);
}

void EventManager::syncFrigateHistory(qint64 hostId)
{
    const QVariantMap info = m_devices->hostInfo(hostId);
    if (!info.value("frigateApi").toBool())
        return;
    const QString host = info.value("addr").toString();
    const int port = info.value("port").toInt();

    QHash<QString, int> channelOf; // raw camera key -> channel
    for (const QVariant &v : m_devices->channelsForHost(hostId)) {
        const QVariantMap m = v.toMap();
        channelOf.insert(m.value("camera").toString(), m.value("channel").toInt());
    }
    if (channelOf.isEmpty())
        return;

    m_pending.addFuture(QtConcurrent::run([this, hostId, host, port, channelOf] {
        QString error;
        const QByteArray body = frigate::httpGet(
            frigate::eventsUrl(host, port, 0.0, 500), 10000, &error);
        const Json parsed = Json::parse(body.toStdString(), nullptr, false);
        if (!error.isEmpty() || parsed.is_discarded() || !parsed.is_array())
            return;
        const QVector<frigate::Event> events = frigate::parseEvents(parsed);
        QMetaObject::invokeMethod(
            this, [this, hostId, channelOf, events] { applyFrigateHistory(hostId, channelOf, events); },
            Qt::QueuedConnection);
    }));
}

void EventManager::checkFrigateCatchup()
{
    for (const QVariant &id : m_devices->hostIds()) {
        const qint64 hostId = id.toLongLong();
        if (m_frigateHistoryChecked.contains(hostId))
            continue;
        const QVariantMap info = m_devices->hostInfo(hostId);
        if (!info.value("frigateApi").toBool() || !info.value("online").toBool())
            continue; // not Frigate, or its cameras aren't discovered yet — try again later
        m_frigateHistoryChecked.insert(hostId);
        if (!m_db->hostHasEvents(hostId))
            syncFrigateHistory(hostId);
    }
}

void EventManager::syncAllFrigateHistory()
{
    for (const QVariant &id : m_devices->hostIds()) {
        const qint64 hostId = id.toLongLong();
        if (m_devices->hostInfo(hostId).value("frigateApi").toBool())
            syncFrigateHistory(hostId);
    }
}

void EventManager::applyFrigateHistory(qint64 hostId, const QHash<QString, int> &channelOf,
                                       const QVector<frigate::Event> &events)
{
    QVector<Inbox::Candidate> batch;
    QVector<QString> frigateIds;   // beside each candidate: Frigate's own event id
    for (const frigate::Event &ev : events) {
        const QString type = frigate::typeForLabel(ev.label);
        if (type.isEmpty() || !channelOf.contains(ev.camera))
            continue;
        batch.append({hostId, channelOf.value(ev.camera), type,
                      frigate::displayCameraName(ev.camera), ev.start});
        frigateIds.append(ev.id);
    }
    QVector<QPair<qint64, QString>> snapshots; // (local db id, Frigate's own event id)
    const QVector<Inbox::Filing> filed = m_inbox.fileHistory(batch);
    for (int i = 0; i < filed.size(); ++i) {
        // A new row, or one already covering this moment (a rerun, or the live poller
        // caught it) whose thumbnail never came.
        if (filed.at(i).needsThumbnail && !m_snapshotsDue.contains(filed.at(i).row.id))
            snapshots.append({filed.at(i).row.id, frigateIds.at(i)});
    }
    take(hostId, filed);
    fetchHistoryThumbnails(hostId, snapshots);
}

void EventManager::fetchHistoryThumbnails(qint64 hostId, const QVector<QPair<qint64, QString>> &ids)
{
    if (ids.isEmpty())
        return;
    const QVariantMap info = m_devices->hostInfo(hostId);
    const QString host = info.value("addr").toString();
    const int port = info.value("port").toInt();
    for (const auto &pair : ids)
        m_snapshotsDue.insert(pair.first);   // a live report of it waits for this one
    m_pending.addFuture(QtConcurrent::run([this, host, port, ids] {
        for (const auto &pair : ids) {
            const qint64 dbId = pair.first;
            const QString &frigateEventId = pair.second;
            const QByteArray jpeg =
                frigate::httpGet(frigate::snapshotUrl(host, port, frigateEventId), 8000);
            const QString path = Paths::thumbnailsDir() + QStringLiteral("/event_%1.jpg").arg(dbId);
            const bool stored = !jpeg.isEmpty()
                && device::storeEventSnapshot(jpeg, path);   // HD, as Reolink's (ticket 11)
            QMetaObject::invokeMethod(
                this, [this, dbId, path, stored] {
                    m_snapshotsDue.remove(dbId);
                    if (stored)
                        onThumbnailReady(dbId, path);
                }, Qt::QueuedConnection);
        }
    }));
}

} // namespace rl
