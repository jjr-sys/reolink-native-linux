#include "EventManager.h"

#include "DeviceManager.h"
#include "device/FrigateApi.h"

#include "core/Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSettings>
#include <QtConcurrent/QtConcurrent>

namespace rl {

// Retention target: events are kept for at least this long, per property — clips
// themselves live on the NVR/Frigate server, so the inbox only costs a DB row plus
// a small JPEG thumbnail per event, and a week of even a busy property's motion
// traffic is a rounding error on disk.
static constexpr qint64 kRetentionSecs = 7 * 24 * 3600;
// Safety valve, applied per host/property: if a stuck sensor or misconfigured
// camera floods far faster than the age purge below can keep up, this stops the
// store growing without bound. 10,000 comfortably covers a week even at the
// busiest rate seen so far (~50 events/hour on one property) with headroom to
// spare — the 7-day window above is what normally governs, this is the backstop.
static constexpr int kPerPropertyCeiling = 10000;
// Sanity bound on how many rows the in-memory model ever loads at once, regardless
// of how many properties/hosts exist.
static constexpr int kLoadLimit = 50000;

// Downscaled JPEG in the thumbnail cache; false if it could not be written. Mirrors
// DeviceManager's saveEventThumbnail() — duplicated rather than shared for a few
// lines that the two call sites (live capture vs. Frigate's own historical
// snapshot) don't otherwise have anything in common to share.
static bool saveThumbnail(const QByteArray &jpeg, const QString &path)
{
    const QImage img = QImage::fromData(jpeg);
    bool ok = false;
    if (!img.isNull())
        ok = img.scaledToWidth(qMin(640, img.width()), Qt::SmoothTransformation)
                 .save(path, "JPG", 85);
    if (!ok) {
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly) && f.write(jpeg) == jpeg.size();
    }
    return ok;
}

EventManager::EventManager(Database *db, DeviceManager *devices, QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_devices(devices)
{
    m_desktopNotifications =
        QSettings().value(QStringLiteral("notifications/desktop"), true).toBool();
    normalizeStoredCameraNames();
    enforceRetention();
    reload();
    connect(m_devices, &DeviceManager::detectionEvent, this, &EventManager::onDetection);
    connect(&m_notifier, &Notifier::activated, this, &EventManager::eventActivated);
    connect(m_devices, &DeviceManager::eventThumbnailReady, this,
            &EventManager::onThumbnailReady);
    connect(m_devices, &DeviceManager::connectivityChanged, this,
            &EventManager::onConnectivity);

    // Age-based purge runs hourly so history still ages out even on a quiet
    // property that never triggers the per-insert safety-valve check.
    m_retentionTimer.setInterval(3600'000);
    connect(&m_retentionTimer, &QTimer::timeout, this, [this] { enforceRetention(); });
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
        m_all.clear();
        m_view.clear();
        for (const auto &m : mock) {
            EventRecord r;
            r.type = QString::fromUtf8(m.type);
            r.camera = QString::fromUtf8(m.cam);
            r.timestamp = now - m.ago;
            m_all.append(r);
        }
        for (int i = 0; i < m_all.size(); ++i)
            if (matches(m_all[i]))
                m_view.append(i);
        endResetModel();
        emit countChanged();
    }
}

EventManager::~EventManager()
{
    m_pending.waitForFinished();
}

bool EventManager::matches(const EventRecord &e) const
{
    const bool typeOk = m_filter.isEmpty() || e.type == m_filter;
    const bool camOk = m_cameraFilter.isEmpty() || e.camera == m_cameraFilter;
    const bool propOk = m_propertyFilter.isEmpty() || m_devices->hostName(e.hostId) == m_propertyFilter;
    return typeOk && camOk && propOk;
}

// Rewrite any Frigate event still stored under its raw config key ("bosch_camera")
// to the pretty form ("Bosch Camera") shown in the filter dropdown and the row —
// only events raised before the display-name fix shipped can still be raw. Safe to
// run every startup: rows already in the pretty form are left untouched.
void EventManager::normalizeStoredCameraNames()
{
    const QVector<EventRecord> events = m_db->recentEvents(kLoadLimit);
    for (const EventRecord &e : events) {
        if (m_devices->hostInfo(e.hostId).value("kind").toString() != QLatin1String("frigate"))
            continue;
        const QString pretty = frigate::displayCameraName(e.camera);
        if (pretty != e.camera)
            m_db->setEventCamera(e.id, pretty);
    }
}

void EventManager::enforceRetention(qint64 hostId)
{
    QStringList thumbs = m_db->purgeEventsOlderThan(kRetentionSecs);
    if (hostId != 0)
        thumbs += m_db->trimHostEvents(hostId, kPerPropertyCeiling);
    if (thumbs.isEmpty())
        return;
    for (const QString &path : std::as_const(thumbs))
        QFile::remove(path);
    m_historyTrimmed = true;
    reload();
}

void EventManager::reload()
{
    beginResetModel();
    m_all = m_db->recentEvents(kLoadLimit);
    m_view.clear();
    for (int i = 0; i < m_all.size(); ++i)
        if (matches(m_all[i]))
            m_view.append(i);
    endResetModel();
    emit countChanged();
}

int EventManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_view.size();
}

QVariant EventManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_view.size())
        return {};
    const EventRecord &e = m_all.at(m_view.at(index.row()));
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

void EventManager::setFilter(const QString &filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    reload();
}

void EventManager::setCameraFilter(const QString &camera)
{
    if (m_cameraFilter == camera)
        return;
    m_cameraFilter = camera;
    emit cameraFilterChanged();
    reload();
}

void EventManager::setPropertyFilter(const QString &property)
{
    if (m_propertyFilter == property)
        return;
    m_propertyFilter = property;
    emit propertyFilterChanged();
    reload();
}

qint64 EventManager::oldestVisibleTimestamp() const
{
    return m_view.isEmpty() ? 0 : m_all.at(m_view.last()).timestamp;
}

void EventManager::onDetection(qint64 hostId, int channel, const QString &type,
                               const QString &camera)
{
    EventRecord rec;
    rec.hostId = hostId;
    rec.channel = channel;
    rec.timestamp = QDateTime::currentSecsSinceEpoch();
    rec.type = type;
    rec.camera = camera;
    rec.id = m_db->addEvent(rec);
    // Grab a frame from the camera for the inbox thumbnail (quiet; the row
    // updates in place when it lands).
    if (rec.id > 0)
        m_devices->captureEventThumbnail(hostId, channel, rec.id);

    m_all.prepend(rec);
    // Shift existing view indices (they point into m_all, which just grew at 0).
    for (int &idx : m_view)
        ++idx;
    if (matches(rec)) {
        beginInsertRows({}, 0, 0);
        m_view.prepend(0);
        endInsertRows();
        emit countChanged();
    }
    ++m_unread;
    emit unreadChanged();

    // Desktop notification for this detection, gated on the camera's Push
    // Notifications being enabled (mirrors how the official apps gate push).
    if (m_desktopNotifications && m_devices->pushEnabledFor(hostId, channel)) {
        QString what;
        if (type == QLatin1String("person"))       what = tr("Person detected");
        else if (type == QLatin1String("vehicle")) what = tr("Vehicle detected");
        else if (type == QLatin1String("pet") || type == QLatin1String("dog_cat"))
            what = tr("Pet detected");
        else if (type == QLatin1String("visitor")) what = tr("Visitor at the door");
        else                                        what = tr("Motion detected");
        m_notifier.notify(camera.isEmpty() ? tr("Camera") : camera, what,
                          QStringLiteral("io.github.todesengelx.ReolinkLinux"),
                          hostId, channel, rec.timestamp);
    }

    // Per-host safety valve only; the age-based purge runs on its own hourly
    // timer. Cheap: a single scoped query, and this host just got an event so
    // it's already at the front of the age ordering.
    enforceRetention(hostId);
}

void EventManager::setDesktopNotifications(bool on)
{
    if (m_desktopNotifications == on)
        return;
    m_desktopNotifications = on;
    QSettings().setValue(QStringLiteral("notifications/desktop"), on);
    emit desktopNotificationsChanged();
}

void EventManager::onConnectivity(qint64 hostId, int channel, const QString &name, bool online)
{
    // Health alert: a camera (channel >= 0) or a whole host dropped or returned.
    // Recorded in the inbox and always notified — a dead camera can't announce
    // itself, so the client has to.
    EventRecord rec;
    rec.hostId = hostId;
    rec.channel = qMax(0, channel);
    rec.timestamp = QDateTime::currentSecsSinceEpoch();
    rec.type = online ? QStringLiteral("online") : QStringLiteral("offline");
    rec.camera = name;
    rec.id = m_db->addEvent(rec);

    m_all.prepend(rec);
    for (int &idx : m_view)
        ++idx;
    if (matches(rec)) {
        beginInsertRows({}, 0, 0);
        m_view.prepend(0);
        endInsertRows();
        emit countChanged();
    }
    ++m_unread;
    emit unreadChanged();

    if (m_desktopNotifications)
    m_notifier.notify(name.isEmpty() ? tr("Device") : name,
                      online ? tr("Back online") : tr("Went offline \u26a0"),
                      QStringLiteral("io.github.todesengelx.ReolinkLinux"),
                      hostId, rec.channel, rec.timestamp);

    enforceRetention(hostId);
}

void EventManager::onThumbnailReady(qint64 eventId, const QString &path)
{
    m_db->setEventThumbnail(eventId, path);
    for (int i = 0; i < m_all.size(); ++i) {
        if (m_all.at(i).id != eventId)
            continue;
        m_all[i].thumbnail = path;
        const int vi = m_view.indexOf(i);
        if (vi >= 0)
            emit dataChanged(index(vi), index(vi), {ThumbnailRole});
        return;
    }
    // Event already trimmed away — don't keep an orphaned file.
    QFile::remove(path);
}

QVariantList EventManager::eventTimesFor(qint64 hostId, int channel, qint64 dayStart) const
{
    QVariantList out;
    const qint64 dayEnd = dayStart + 86400;
    for (const EventRecord &e : m_all) {
        if (e.hostId != hostId || e.channel != channel)
            continue;
        if (e.timestamp < dayStart || e.timestamp >= dayEnd)
            continue;
        if (e.type == QLatin1String("offline") || e.type == QLatin1String("online"))
            continue; // connectivity events aren't recordings
        out.append(int(e.timestamp - dayStart));
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
    m_unread = 0;
    emit unreadChanged();
    reload();
}

void EventManager::syncFrigateHistory(qint64 hostId)
{
    const QVariantMap info = m_devices->hostInfo(hostId);
    if (info.value("kind").toString() != QLatin1String("frigate"))
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
        if (info.value("kind").toString() != QLatin1String("frigate") || !info.value("online").toBool())
            continue; // not Frigate, or its cameras aren't discovered yet — try again later
        m_frigateHistoryChecked.insert(hostId);
        bool hasAny = false;
        for (const EventRecord &e : m_all)
            if (e.hostId == hostId) { hasAny = true; break; }
        if (!hasAny)
            syncFrigateHistory(hostId);
    }
}

void EventManager::syncAllFrigateHistory()
{
    for (const QVariant &id : m_devices->hostIds()) {
        const qint64 hostId = id.toLongLong();
        if (m_devices->hostInfo(hostId).value("kind").toString() == QLatin1String("frigate"))
            syncFrigateHistory(hostId);
    }
}

void EventManager::applyFrigateHistory(qint64 hostId, const QHash<QString, int> &channelOf,
                                       const QVector<frigate::Event> &events)
{
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - kRetentionSecs;
    QHash<QString, double> lastRaised; // "camera/type" -> last raised start, same burst
                                        // gate as the live poller so a lingering person
                                        // doesn't become a dozen separate rows.
    bool added = false;
    QVector<QPair<qint64, QString>> newThumbs; // (local db id, Frigate's own event id)
    // Frigate returns newest first; walk oldest first so the burst gate sees them
    // in the order they actually happened.
    for (int i = events.size() - 1; i >= 0; --i) {
        const frigate::Event &ev = events.at(i);
        if (ev.start < cutoff || !channelOf.contains(ev.camera))
            continue;
        const QString type = frigate::typeForLabel(ev.label);
        if (type.isEmpty())
            continue;
        const QString key = ev.camera + u'/' + type;
        double &last = lastRaised[key];
        if (!frigate::startsNewBurst(last, ev.start))
            continue;
        last = ev.start;

        const int channel = channelOf.value(ev.camera);
        const qint64 ts = static_cast<qint64>(ev.start);
        // Skip anything that already covers this moment — a rerun of the sync, or
        // the live poller having already caught it — but if that existing row is
        // still missing its thumbnail (e.g. it was filed before this sync could
        // fetch one), pick that up now rather than leaving it blank forever.
        bool dup = false;
        for (const EventRecord &e : m_all) {
            if (e.hostId == hostId && e.channel == channel
                && qAbs(e.timestamp - ts) < qint64(frigate::kBurstGapSecs)) {
                dup = true;
                if (e.thumbnail.isEmpty())
                    newThumbs.append({e.id, ev.id});
                break;
            }
        }
        if (dup)
            continue;

        EventRecord rec;
        rec.hostId = hostId;
        rec.channel = channel;
        rec.timestamp = ts;
        rec.type = type;
        rec.camera = frigate::displayCameraName(ev.camera);
        const qint64 dbId = m_db->addEvent(rec);
        if (dbId > 0) {
            added = true;
            newThumbs.append({dbId, ev.id});
        }
    }
    if (added)
        reload();
    fetchHistoryThumbnails(hostId, newThumbs);
}

void EventManager::fetchHistoryThumbnails(qint64 hostId, const QVector<QPair<qint64, QString>> &ids)
{
    if (ids.isEmpty())
        return;
    const QVariantMap info = m_devices->hostInfo(hostId);
    const QString host = info.value("addr").toString();
    const int port = info.value("port").toInt();
    m_pending.addFuture(QtConcurrent::run([this, host, port, ids] {
        for (const auto &pair : ids) {
            const qint64 dbId = pair.first;
            const QString &frigateEventId = pair.second;
            const QByteArray jpeg =
                frigate::httpGet(frigate::snapshotUrl(host, port, frigateEventId), 8000);
            if (jpeg.isEmpty())
                continue;
            const QString path = Paths::thumbnailsDir() + QStringLiteral("/event_%1.jpg").arg(dbId);
            if (!saveThumbnail(jpeg, path))
                continue;
            QMetaObject::invokeMethod(
                this, [this, dbId, path] { onThumbnailReady(dbId, path); }, Qt::QueuedConnection);
        }
    }));
}

} // namespace rl
