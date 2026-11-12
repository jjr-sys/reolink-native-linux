#include "Database.h"

#include "Log.h"

#include <QDateTime>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace rl {

// A null QString binds as SQL NULL, which violates our NOT NULL text columns;
// the schema wants empty strings.
static QString sqlText(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

Database::Database(const QString &filePath, const QString &connectionName)
    : m_filePath(filePath), m_connectionName(connectionName)
{
}

Database::~Database()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::database(m_connectionName).close();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

QSqlDatabase Database::db() const
{
    return QSqlDatabase::database(m_connectionName);
}

bool Database::open()
{
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(m_filePath);
    if (!database.open()) {
        m_lastError = database.lastError().text();
        qCCritical(lcCore) << "Failed to open database" << m_filePath << ":" << m_lastError;
        return false;
    }
    // Owner-only permissions: no secrets live here (credentials are in the
    // keyring), but the device list / event history is still private.
    QFile::setPermissions(m_filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QSqlQuery pragma(database);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    return migrate();
}

int Database::schemaVersion() const
{
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT version FROM schema_version")) && q.next())
        return q.value(0).toInt();
    return 0;
}

bool Database::migrate()
{
    static const QStringList migrationsV1 = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS hosts ("
                       " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       " kind TEXT NOT NULL,"
                       " name TEXT NOT NULL,"
                       " addr TEXT NOT NULL,"
                       " port INTEGER NOT NULL DEFAULT 443,"
                       " https INTEGER NOT NULL DEFAULT 1,"
                       " username TEXT NOT NULL DEFAULT '',"
                       " model TEXT NOT NULL DEFAULT '',"
                       " created_at TEXT NOT NULL DEFAULT (datetime('now')))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS channels ("
                       " host_id INTEGER NOT NULL REFERENCES hosts(id) ON DELETE CASCADE,"
                       " idx INTEGER NOT NULL,"
                       " name TEXT NOT NULL DEFAULT '',"
                       " model TEXT NOT NULL DEFAULT '',"
                       " PRIMARY KEY (host_id, idx))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS layouts ("
                       " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       " name TEXT NOT NULL,"
                       " preset INTEGER NOT NULL DEFAULT 4,"
                       " panes_json TEXT NOT NULL DEFAULT '[]')"),
    };

    static const QStringList migrationsV2 = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS events ("
                       " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       " host_id INTEGER NOT NULL,"
                       " channel INTEGER NOT NULL DEFAULT 0,"
                       " ts INTEGER NOT NULL,"
                       " type TEXT NOT NULL,"
                       " camera TEXT NOT NULL DEFAULT '',"
                       " thumbnail TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts DESC)"),
    };

    // Events storage (wayfinder/site-tabs-redesign step 5): per-camera day queries, app
    // runs for the "app not running" bands, and each channel's supported event types.
    static const QStringList migrationsV3 = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_events_camera"
                       " ON events(host_id, channel, type, ts)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS app_runs ("
                       " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       " started_at INTEGER NOT NULL,"
                       " last_seen INTEGER NOT NULL)"),
        QStringLiteral("ALTER TABLE channels ADD COLUMN event_types TEXT NOT NULL DEFAULT ''"),
    };

    // Event switches (wayfinder/events-feed): the (recorder, camera, type) pairs switched
    // off, which the app neither stores nor shows. Kept here only, never on a recorder.
    static const QStringList migrationsV4 = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS event_switches_off ("
                       " host_id INTEGER NOT NULL,"
                       " channel INTEGER NOT NULL,"
                       " type TEXT NOT NULL,"
                       " PRIMARY KEY (host_id, channel, type))"),
    };

    // Unseen counts (wayfinder/events-feed step 20): when each camera was last opened in the
    // Events page on this desktop. Every camera already known starts seen now, so the
    // upgrade shows nothing unseen; a camera added later has no row, which counts all it
    // logs (the same as starting it when first seen, since it has logged nothing before).
    static const QStringList migrationsV5 = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS camera_seen ("
                       " host_id INTEGER NOT NULL,"
                       " channel INTEGER NOT NULL,"
                       " seen_at INTEGER NOT NULL,"
                       " PRIMARY KEY (host_id, channel))"),
        // A recorder whose clock runs ahead has events stamped after now: seen up to them.
        QStringLiteral("INSERT OR IGNORE INTO camera_seen (host_id, channel, seen_at)"
                       " SELECT host_id, channel,"
                       " MAX(MAX(ts), CAST(strftime('%s', 'now') AS INTEGER))"
                       " FROM events GROUP BY host_id, channel"),
        QStringLiteral("INSERT OR IGNORE INTO camera_seen (host_id, channel, seen_at)"
                       " SELECT host_id, idx, CAST(strftime('%s', 'now') AS INTEGER) FROM channels"),
    };

    // Ordered migrations; each bumps the stored version by one.
    const QVector<QPair<int, const QStringList *>> steps = {
        {1, &migrationsV1},
        {2, &migrationsV2},
        {3, &migrationsV3},
        {4, &migrationsV4},
        {5, &migrationsV5},
    };

    const int current = schemaVersion();
    QSqlDatabase database = db();
    for (const auto &[version, stmts] : steps) {
        if (current >= version)
            continue;
        if (!database.transaction()) {
            m_lastError = database.lastError().text();
            return false;
        }
        for (const QString &stmt : *stmts) {
            QSqlQuery q(database);
            if (!q.exec(stmt)) {
                m_lastError = q.lastError().text();
                qCCritical(lcCore) << "Migration to v" << version << "failed:" << m_lastError;
                database.rollback();
                return false;
            }
        }
        QSqlQuery q(database);
        q.exec(QStringLiteral("DELETE FROM schema_version"));
        q.prepare(QStringLiteral("INSERT INTO schema_version (version) VALUES (:v)"));
        q.bindValue(QStringLiteral(":v"), version);
        q.exec();
        if (!database.commit()) {
            m_lastError = database.lastError().text();
            database.rollback();
            return false;
        }
        qCInfo(lcCore) << "Database migrated to schema v" << version;
    }
    return true;
}

QVector<EventSwitchKey> Database::switchedOffEvents() const
{
    QVector<EventSwitchKey> out;
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("SELECT host_id, channel, type FROM event_switches_off")))
        return out;
    while (q.next())
        out.append({q.value(0).toLongLong(), q.value(1).toInt(), q.value(2).toString()});
    return out;
}

bool Database::setEventSwitchedOff(const EventSwitchKey &key, bool off)
{
    QSqlQuery q(db());
    q.prepare(off ? QStringLiteral("INSERT OR IGNORE INTO event_switches_off"
                                   " (host_id, channel, type) VALUES (:h, :c, :t)")
                  : QStringLiteral("DELETE FROM event_switches_off"
                                   " WHERE host_id = :h AND channel = :c AND type = :t"));
    q.bindValue(QStringLiteral(":h"), key.hostId);
    q.bindValue(QStringLiteral(":c"), key.channel);
    q.bindValue(QStringLiteral(":t"), key.type);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

qint64 Database::addEvent(const EventRecord &rec)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO events (host_id, channel, ts, type, camera, thumbnail)"
        " VALUES (:host, :ch, :ts, :type, :cam, :thumb)"));
    q.bindValue(QStringLiteral(":host"), rec.hostId);
    q.bindValue(QStringLiteral(":ch"), rec.channel);
    q.bindValue(QStringLiteral(":ts"), rec.timestamp);
    q.bindValue(QStringLiteral(":type"), rec.type);
    q.bindValue(QStringLiteral(":cam"), sqlText(rec.camera));
    q.bindValue(QStringLiteral(":thumb"), sqlText(rec.thumbnail));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

static EventRecord eventFrom(const QSqlQuery &q)
{
    EventRecord r;
    r.id = q.value(0).toLongLong();
    r.hostId = q.value(1).toLongLong();
    r.channel = q.value(2).toInt();
    r.timestamp = q.value(3).toLongLong();
    r.type = q.value(4).toString();
    r.camera = q.value(5).toString();
    r.thumbnail = q.value(6).toString();
    return r;
}

static const char kEventColumns[] = "SELECT id, host_id, channel, ts, type, camera, thumbnail FROM events";

QVector<EventRecord> Database::recentEvents(int limit, const QString &type,
                                            const QList<qint64> &hostIds,
                                            const QString &camera) const
{
    QVector<EventRecord> out;
    QStringList where;
    if (!type.isEmpty())
        where << QStringLiteral("type = :type");
    if (!camera.isEmpty())
        where << QStringLiteral("camera = :cam");
    if (!hostIds.isEmpty()) {
        QStringList ids;
        for (qint64 id : hostIds)
            ids << QString::number(id);
        where << QStringLiteral("host_id IN (%1)").arg(ids.join(QLatin1Char(',')));
    }
    QString sql = QString::fromLatin1(kEventColumns);
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY ts DESC, id DESC LIMIT :lim");
    QSqlQuery q(db());
    q.prepare(sql);
    if (!type.isEmpty())
        q.bindValue(QStringLiteral(":type"), type);
    if (!camera.isEmpty())
        q.bindValue(QStringLiteral(":cam"), camera);
    q.bindValue(QStringLiteral(":lim"), limit);
    if (!q.exec())
        return out;
    while (q.next())
        out.append(eventFrom(q));
    return out;
}

QVector<EventRecord> Database::eventsBetween(qint64 hostId, int channel, qint64 from, qint64 to,
                                             const QStringList &types) const
{
    QVector<EventRecord> out;
    QString sql = QString::fromLatin1(kEventColumns)
                  + QStringLiteral(" WHERE host_id = :h AND channel = :c AND ts >= :from AND ts < :to");
    if (!types.isEmpty()) {
        QStringList marks;
        for (int i = 0; i < types.size(); ++i)
            marks << QStringLiteral(":t%1").arg(i);
        sql += QStringLiteral(" AND type IN (%1)").arg(marks.join(QLatin1Char(',')));
    }
    sql += QStringLiteral(" ORDER BY ts DESC, id DESC");
    QSqlQuery q(db());
    q.prepare(sql);
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":c"), channel);
    q.bindValue(QStringLiteral(":from"), from);
    q.bindValue(QStringLiteral(":to"), to);
    for (int i = 0; i < types.size(); ++i)
        q.bindValue(QStringLiteral(":t%1").arg(i), types.at(i));
    if (!q.exec())
        return out;
    while (q.next())
        out.append(eventFrom(q));
    return out;
}

QHash<QString, int> Database::eventCountsBetween(qint64 hostId, int channel, qint64 from,
                                                 qint64 to) const
{
    QHash<QString, int> out;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT type, COUNT(*) FROM events WHERE host_id = :h AND channel = :c"
                             " AND ts >= :from AND ts < :to GROUP BY type"));
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":c"), channel);
    q.bindValue(QStringLiteral(":from"), from);
    q.bindValue(QStringLiteral(":to"), to);
    if (q.exec())
        while (q.next())
            out.insert(q.value(0).toString(), q.value(1).toInt());
    return out;
}

QHash<qint64, QHash<QString, int>> Database::eventCountsByHost(qint64 from, qint64 to) const
{
    QHash<qint64, QHash<QString, int>> out;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT host_id, type, COUNT(*) FROM events WHERE ts >= :from"
                             " AND ts < :to GROUP BY host_id, type"));
    q.bindValue(QStringLiteral(":from"), from);
    q.bindValue(QStringLiteral(":to"), to);
    if (q.exec())
        while (q.next())
            out[q.value(0).toLongLong()].insert(q.value(1).toString(), q.value(2).toInt());
    return out;
}

bool Database::hostHasEvents(qint64 hostId) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT 1 FROM events WHERE host_id = :h LIMIT 1"));
    q.bindValue(QStringLiteral(":h"), hostId);
    return q.exec() && q.next();
}

std::optional<EventRecord> Database::eventNear(qint64 hostId, int channel, qint64 ts,
                                               qint64 windowSecs) const
{
    QSqlQuery q(db());
    q.prepare(QString::fromLatin1(kEventColumns)
              + QStringLiteral(" WHERE host_id = :h AND channel = :c AND ts > :lo AND ts < :hi"
                               " ORDER BY ABS(ts - :ts) LIMIT 1"));
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":c"), channel);
    q.bindValue(QStringLiteral(":lo"), ts - windowSecs);
    q.bindValue(QStringLiteral(":hi"), ts + windowSecs);
    q.bindValue(QStringLiteral(":ts"), ts);
    if (q.exec() && q.next())
        return eventFrom(q);
    return std::nullopt;
}

QStringList Database::eventCameras(qint64 hostId) const
{
    QStringList out;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT DISTINCT camera FROM events WHERE host_id = :h"));
    q.bindValue(QStringLiteral(":h"), hostId);
    if (q.exec())
        while (q.next())
            out.append(q.value(0).toString());
    return out;
}

QHash<QPair<qint64, int>, QString> Database::eventCameraNames() const
{
    QHash<QPair<qint64, int>, QString> out;
    QSqlQuery q(db());
    // SQLite returns the bare `camera` from the row holding MAX(ts). Status types as in
    // EventSwitches::isStatus.
    if (q.exec(QStringLiteral("SELECT host_id, channel, camera, MAX(ts) FROM events"
                              " WHERE type NOT IN ('online', 'offline')"
                              " GROUP BY host_id, channel")))
        while (q.next())
            out.insert({q.value(0).toLongLong(), q.value(1).toInt()}, q.value(2).toString());
    return out;
}

QHash<QPair<qint64, int>, qint64> Database::lastEventTimes() const
{
    QHash<QPair<qint64, int>, qint64> out;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT host_id, channel, MAX(ts) FROM events"
                              " WHERE type NOT IN ('online', 'offline')"
                              " GROUP BY host_id, channel")))
        while (q.next())
            out.insert({q.value(0).toLongLong(), q.value(1).toInt()}, q.value(2).toLongLong());
    return out;
}

QHash<QPair<qint64, int>, qint64> Database::cameraSeenTimes() const
{
    QHash<QPair<qint64, int>, qint64> out;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT host_id, channel, seen_at FROM camera_seen")))
        while (q.next())
            out.insert({q.value(0).toLongLong(), q.value(1).toInt()}, q.value(2).toLongLong());
    return out;
}

bool Database::setCameraSeen(qint64 hostId, int channel, qint64 at)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO camera_seen (host_id, channel, seen_at) VALUES (:h, :c, :t)"
                             " ON CONFLICT (host_id, channel) DO UPDATE SET seen_at = excluded.seen_at"));
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":c"), channel);
    q.bindValue(QStringLiteral(":t"), at);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

QHash<QString, int> Database::eventCountsAfter(qint64 hostId, int channel, qint64 after,
                                               const QStringList &types) const
{
    QHash<QString, int> out;
    if (types.isEmpty())
        return out;
    QStringList marks;
    for (int i = 0; i < types.size(); ++i)
        marks << QStringLiteral(":t%1").arg(i);
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT type, COUNT(*) FROM events WHERE host_id = :h AND channel = :c"
                             " AND ts > :after AND type IN (%1) GROUP BY type")
                  .arg(marks.join(QStringLiteral(", "))));
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":c"), channel);
    q.bindValue(QStringLiteral(":after"), after);
    for (int i = 0; i < types.size(); ++i)
        q.bindValue(marks.at(i), types.at(i));
    if (q.exec())
        while (q.next())
            out.insert(q.value(0).toString(), q.value(1).toInt());
    return out;
}

bool Database::renameEventCamera(qint64 hostId, const QString &from, const QString &to)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE events SET camera = :to WHERE host_id = :h AND camera = :from"));
    q.bindValue(QStringLiteral(":to"), sqlText(to));
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":from"), sqlText(from));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool Database::clearEvents()
{
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("DELETE FROM events"))) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool Database::setEventThumbnail(qint64 id, const QString &path)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE events SET thumbnail = :thumb WHERE id = :id"));
    q.bindValue(QStringLiteral(":thumb"), path);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return q.numRowsAffected() > 0;
}

bool Database::setEventCamera(qint64 id, const QString &camera)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE events SET camera = :cam WHERE id = :id"));
    q.bindValue(QStringLiteral(":cam"), camera);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

// Runs `whereSql` (with its bindings) as a delete, first collecting the thumbnails of
// the rows it removes.
static QStringList deleteEventsWhere(const QSqlDatabase &database, const QString &whereSql,
                                     const QVariantMap &binds, QString *error)
{
    QStringList thumbs;
    QSqlQuery sel(database);
    sel.prepare(QStringLiteral("SELECT thumbnail FROM events WHERE thumbnail != '' AND ") + whereSql);
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
        sel.bindValue(it.key(), it.value());
    if (sel.exec())
        while (sel.next())
            thumbs.append(sel.value(0).toString());

    QSqlQuery del(database);
    del.prepare(QStringLiteral("DELETE FROM events WHERE ") + whereSql);
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
        del.bindValue(it.key(), it.value());
    if (!del.exec())
        *error = del.lastError().text();
    return thumbs;
}

QStringList Database::purgeExpiredEvents(qint64 now, qint64 motionKeepSecs, qint64 otherKeepSecs)
{
    return deleteEventsWhere(
        db(),
        QStringLiteral("((type = 'motion' AND ts < :mcut) OR (type <> 'motion' AND ts < :ocut))"),
        {{QStringLiteral(":mcut"), now - motionKeepSecs}, {QStringLiteral(":ocut"), now - otherKeepSecs}},
        &m_lastError);
}

int Database::countCappedEvents(qint64 hostId) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM events WHERE host_id = :h AND type <> 'motion'"));
    q.bindValue(QStringLiteral(":h"), hostId);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

QStringList Database::trimHostEvents(qint64 hostId, int keep)
{
    // The newest row past the cap; it and everything older go. One indexed lookup instead
    // of a NOT IN over the kept rows.
    QSqlQuery cut(db());
    cut.prepare(QStringLiteral("SELECT ts, id FROM events WHERE host_id = :h AND type <> 'motion'"
                               " ORDER BY ts DESC, id DESC LIMIT 1 OFFSET :keep"));
    cut.bindValue(QStringLiteral(":h"), hostId);
    cut.bindValue(QStringLiteral(":keep"), keep);
    if (!cut.exec() || !cut.next())
        return {};
    return deleteEventsWhere(
        db(),
        QStringLiteral("host_id = :h AND type <> 'motion' AND (ts < :ts OR (ts = :ts AND id <= :id))"),
        {{QStringLiteral(":h"), hostId},
         {QStringLiteral(":ts"), cut.value(0).toLongLong()},
         {QStringLiteral(":id"), cut.value(1).toLongLong()}},
        &m_lastError);
}

int Database::countEvents(qint64 hostId, int channel, const QString &type,
                          QStringList *thumbnails) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT thumbnail FROM events"
                             " WHERE host_id = :h AND channel = :c AND type = :t"));
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":c"), channel);
    q.bindValue(QStringLiteral(":t"), type);
    if (!q.exec())
        return 0;
    int n = 0;
    while (q.next()) {
        ++n;
        const QString thumb = q.value(0).toString();
        if (thumbnails && !thumb.isEmpty())
            thumbnails->append(thumb);
    }
    return n;
}

bool Database::deleteEvents(qint64 hostId, int channel, const QString &type,
                            QStringList *thumbnails)
{
    QString error;
    const QStringList thumbs = deleteEventsWhere(
        db(), QStringLiteral("host_id = :h AND channel = :c AND type = :t"),
        {{QStringLiteral(":h"), hostId}, {QStringLiteral(":c"), channel}, {QStringLiteral(":t"), type}},
        &error);
    if (!error.isEmpty()) {
        m_lastError = error;
        return false; // the rows are still there, so their snapshots must stay too
    }
    if (thumbnails)
        *thumbnails = thumbs;
    return true;
}

qint64 Database::beginAppRun(qint64 now)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO app_runs (started_at, last_seen) VALUES (:t, :t)"));
    q.bindValue(QStringLiteral(":t"), now);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool Database::touchAppRun(qint64 id, qint64 now)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE app_runs SET last_seen = :t WHERE id = :id"));
    q.bindValue(QStringLiteral(":t"), now);
    q.bindValue(QStringLiteral(":id"), id);
    return q.exec() && q.numRowsAffected() > 0;
}

QVector<std::pair<qint64, qint64>> Database::appRuns(qint64 from, qint64 to) const
{
    QVector<std::pair<qint64, qint64>> out;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT started_at, last_seen FROM app_runs"
                             " WHERE started_at < :to AND last_seen > :from ORDER BY started_at"));
    q.bindValue(QStringLiteral(":from"), from);
    q.bindValue(QStringLiteral(":to"), to);
    if (q.exec())
        while (q.next())
            out.append({q.value(0).toLongLong(), q.value(1).toLongLong()});
    return out;
}

qint64 Database::firstAppRunStart() const
{
    QSqlQuery q(db());
    return q.exec(QStringLiteral("SELECT MIN(started_at) FROM app_runs")) && q.next()
               ? q.value(0).toLongLong() : 0;
}

qint64 Database::addHost(const HostRecord &rec)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO hosts (kind, name, addr, port, https, username, model)"
        " VALUES (:kind, :name, :addr, :port, :https, :username, :model)"));
    q.bindValue(QStringLiteral(":kind"), rec.kind);
    q.bindValue(QStringLiteral(":name"), sqlText(rec.name));
    q.bindValue(QStringLiteral(":addr"), rec.addr);
    q.bindValue(QStringLiteral(":port"), rec.port);
    q.bindValue(QStringLiteral(":https"), rec.https ? 1 : 0);
    q.bindValue(QStringLiteral(":username"), sqlText(rec.username));
    q.bindValue(QStringLiteral(":model"), sqlText(rec.model));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool Database::removeHost(qint64 id)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM hosts WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool Database::updateHost(const HostRecord &rec)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE hosts SET kind=:kind, name=:name, addr=:addr, port=:port, https=:https,"
        " username=:username, model=:model WHERE id=:id"));
    q.bindValue(QStringLiteral(":kind"), rec.kind);
    q.bindValue(QStringLiteral(":name"), sqlText(rec.name));
    q.bindValue(QStringLiteral(":addr"), rec.addr);
    q.bindValue(QStringLiteral(":port"), rec.port);
    q.bindValue(QStringLiteral(":https"), rec.https ? 1 : 0);
    q.bindValue(QStringLiteral(":username"), sqlText(rec.username));
    q.bindValue(QStringLiteral(":model"), sqlText(rec.model));
    q.bindValue(QStringLiteral(":id"), rec.id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

QVector<HostRecord> Database::hosts() const
{
    QVector<HostRecord> out;
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral(
            "SELECT id, kind, name, addr, port, https, username, model FROM hosts ORDER BY id")))
        return out;
    while (q.next()) {
        HostRecord r;
        r.id = q.value(0).toLongLong();
        r.kind = q.value(1).toString();
        r.name = q.value(2).toString();
        r.addr = q.value(3).toString();
        r.port = q.value(4).toInt();
        r.https = q.value(5).toInt() != 0;
        r.username = q.value(6).toString();
        r.model = q.value(7).toString();
        out.append(r);
    }
    return out;
}

QHash<int, QString> Database::channelNameOverrides(qint64 hostId) const
{
    QHash<int, QString> out;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT idx, name FROM channels WHERE host_id = :h AND name <> ''"));
    q.bindValue(QStringLiteral(":h"), hostId);
    if (q.exec())
        while (q.next())
            out.insert(q.value(0).toInt(), q.value(1).toString());
    return out;
}

bool Database::setChannelNameOverride(qint64 hostId, int channel, const QString &name)
{
    QSqlQuery q(db());
    if (name.trimmed().isEmpty()) {
        // Keep the row: it also holds the channel's supported event types.
        q.prepare(QStringLiteral("UPDATE channels SET name = '' WHERE host_id = :h AND idx = :i"));
    } else {
        q.prepare(QStringLiteral("INSERT INTO channels (host_id, idx, name) VALUES (:h, :i, :n)"
                                 " ON CONFLICT(host_id, idx) DO UPDATE SET name = excluded.name"));
        q.bindValue(QStringLiteral(":n"), name.trimmed());
    }
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":i"), channel);
    return q.exec();
}

QHash<int, QStringList> Database::channelEventTypes(qint64 hostId) const
{
    QHash<int, QStringList> out;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT idx, event_types FROM channels WHERE host_id = :h AND event_types <> ''"));
    q.bindValue(QStringLiteral(":h"), hostId);
    if (q.exec())
        while (q.next())
            out.insert(q.value(0).toInt(), q.value(1).toString().split(QLatin1Char(',')));
    return out;
}

bool Database::setChannelEventTypes(qint64 hostId, int channel, const QStringList &types)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO channels (host_id, idx, event_types) VALUES (:h, :i, :t)"
                             " ON CONFLICT(host_id, idx) DO UPDATE SET event_types = excluded.event_types"));
    q.bindValue(QStringLiteral(":h"), hostId);
    q.bindValue(QStringLiteral(":i"), channel);
    q.bindValue(QStringLiteral(":t"), types.join(QLatin1Char(',')));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

} // namespace rl
