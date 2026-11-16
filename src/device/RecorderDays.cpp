#include "RecorderDays.h"

#include "device/FrigateApi.h"
#include "protocol/ReolinkApi.h"
#include "protocol/ReolinkTransport.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimeZone>
#include <QVariantMap>

namespace rl {

namespace {
constexpr int kFrigateTimeoutMs = 20000;   // each of Frigate's two requests
} // namespace

QVariantList daySegments(const QVector<DayFile> &files, const QDate &date)
{
    const double dayStart = double(date.startOfDay().toSecsSinceEpoch());
    QVariantList out;
    for (const DayFile &f : files) {
        const double s = qBound(0.0, f.startEpoch - dayStart, 86400.0);
        const double e = qBound(s, f.endEpoch - dayStart, 86400.0);
        QVariantMap seg;
        seg["start"] = s;
        seg["end"] = e;
        seg["type"] = QStringLiteral("timer");
        seg["startEpoch"] = f.startEpoch;
        out.append(seg);
    }
    return out;
}

DaySearch reolinkDaySearch(std::shared_ptr<ReolinkTransport> transport, int channel)
{
    return [transport, channel](const QDate &date, const std::atomic<bool> *) {
        DayAnswer a;
        const api::BatchResult batch = transport->call(Json::array({api::searchBody(
            channel, QDateTime(date, QTime(0, 0, 0)), QDateTime(date, QTime(23, 59, 59)),
            QStringLiteral("sub"))}));
        if (!batch.transportOk || batch.results.isEmpty() || !batch.results.first().ok) {
            a.error = batch.error.isEmpty() ? QObject::tr("search failed") : batch.error;
            return a;
        }
        const api::SearchResult sr = api::parseSearch(batch.results.first().value);
        for (const api::RecordingFile &f : sr.files)
            if (f.start.isValid())
                a.files.append({double(f.start.toSecsSinceEpoch()), double(f.end.toSecsSinceEpoch())});
        for (int d : sr.recordingDays)
            a.monthDays.append(d);
        return a;
    };
}

DaySearch frigateDaySearch(const QString &host, int port, const QString &camera)
{
    return [host, port, camera](const QDate &date, const std::atomic<bool> *cancel) {
        DayAnswer a;
        const qint64 after = date.startOfDay().toSecsSinceEpoch();
        const qint64 before = date.addDays(1).startOfDay().toSecsSinceEpoch();
        QString error;
        const QByteArray body = frigate::httpGet(
            frigate::recordingsUrl(host, port, camera, after, before), kFrigateTimeoutMs, &error, cancel);
        const Json parsed = Json::parse(body.toStdString(), nullptr, false);
        if (!error.isEmpty() || parsed.is_discarded() || !parsed.is_array()) {
            a.error = error.isEmpty() ? QObject::tr("unexpected reply from Frigate") : error;
            return a;
        }
        for (const frigate::Recording &r : frigate::mergeRecordings(frigate::parseRecordings(parsed)))
            a.files.append({r.start, r.end});
        // The month's days with footage: a missing summary only leaves the calendar unmarked.
        const QString zone = QString::fromLatin1(QTimeZone::systemTimeZoneId());
        QString ignored;
        const QByteArray sumBody = frigate::httpGet(frigate::summaryUrl(host, port, camera, zone),
                                                    kFrigateTimeoutMs, &ignored, cancel);
        const Json sum = Json::parse(sumBody.toStdString(), nullptr, false);
        if (!sum.is_discarded())
            a.monthDays = frigate::parseRecordingDays(sum, date.year(), date.month());
        return a;
    };
}

namespace {
// Searches at once: a few cameras' days (Playback's selection, Live replay's set); a recorder
// queues its own.
constexpr int kWorkers = 4;
// How long the module going waits for its cancelled searches (they stop within ~50 ms).
constexpr int kGoingWaitMs = 2000;
} // namespace

RecorderDays::RecorderDays(Sources sources, QObject *parent)
    : QObject(parent), m_sources(std::move(sources)), m_pool(std::make_unique<QThreadPool>())
{
    m_pool->setMaxThreadCount(kWorkers);
}

RecorderDays::~RecorderDays()
{
    m_cancel->store(true);
    if (!m_pool->waitForDone(kGoingWaitMs)) {
        // Never hold up quitting: the workers hold only their own copies and end on their own.
        qWarning("RecorderDays: a cancelled search didn't stop in %d ms; not waiting", kGoingWaitMs);
        m_pool.release();
    }
}

void RecorderDays::search(const QString &key, int year, int month, int day)
{
    const QDate date(year, month, day);
    const DaySearch source = date.isValid() ? m_sources(key) : DaySearch{};
    if (!source) {
        // Queued, as a search's answer is: callers never see it inside their own call.
        QMetaObject::invokeMethod(this, [this, key, year, month, day] {
            emit dayFailed(key, year, month, day, tr("device not ready"));
        }, Qt::QueuedConnection);
        return;
    }
    QPointer<RecorderDays> self(this);
    const auto cancel = m_cancel;
    m_pool->start([self, cancel, source, key, date] {
        const DayAnswer a = source(date, cancel.get());
        if (cancel->load())
            return;   // the module went: nobody to answer
        const QVariantList segments = a.error.isEmpty() ? daySegments(a.files, date) : QVariantList{};
        QVariantList monthDays;
        for (int d : a.monthDays)
            monthDays.append(d);
        QMetaObject::invokeMethod(self, [self, key, date, a, segments, monthDays] {
            if (!self)
                return;
            if (a.error.isEmpty())
                emit self->dayFound(key, date.year(), date.month(), date.day(), segments, monthDays);
            else
                emit self->dayFailed(key, date.year(), date.month(), date.day(), a.error);
        }, Qt::QueuedConnection);
    });
}

} // namespace rl
