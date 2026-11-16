#pragma once

#include <QDate>
#include <QObject>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QThreadPool;

namespace rl {

class ReolinkTransport;

// One recording, epoch seconds.
struct DayFile
{
    double startEpoch = 0;
    double endEpoch = 0;
};

// A day's recordings as the Playback timeline and Live replay take them: {start, end} in
// seconds into the local `date` (clamped to 0..86400), type "timer", and the recording's own
// `startEpoch`.
QVariantList daySegments(const QVector<DayFile> &files, const QDate &date);

// One camera's answer for a day: its recordings and the days of that month with footage, or
// why it couldn't say (`error` not empty).
struct DayAnswer
{
    QString error;
    QVector<DayFile> files;
    QList<int> monthDays;
};

// One camera's day search, blocking, for a worker thread. `cancel` (may be null) stops it
// early: the answer is then thrown away.
using DaySearch = std::function<DayAnswer(const QDate &date, const std::atomic<bool> *cancel)>;

// A Reolink camera or recorder channel: one sub-stream Search over the local day (HTTP CGI).
DaySearch reolinkDaySearch(std::shared_ptr<ReolinkTransport> transport, int channel);
// A Frigate camera (`camera` as Frigate names it): its recordings over the local day and the
// month's days with footage from its summary (in this machine's time zone).
DaySearch frigateDaySearch(const QString &host, int port, const QString &camera);

// One camera's day of recordings, from whichever vendor holds them (wayfinder/architecture
// ticket 01). Ask with search(key, y, m, d); the answer always names the camera key
// ("hostId:channel") and the day, so a page takes only answers for a camera and day it shows:
// dayFound (its recordings), dayFailed (the device said no, or no such day) or dayNotReady
// (the camera can't be asked yet; ticket 07).
class RecorderDays : public QObject
{
    Q_OBJECT
public:
    // The camera's search, or an empty one when it can't be searched now (device not ready:
    // answered with dayNotReady).
    using Sources = std::function<DaySearch(const QString &key)>;
    explicit RecorderDays(Sources sources, QObject *parent = nullptr);
    ~RecorderDays() override;   // stops searches in flight, waits at most 2 s

    Q_INVOKABLE void search(const QString &key, int year, int month, int day);

signals:
    void dayFound(const QString &key, int year, int month, int day, const QVariantList &segments,
                  const QVariantList &monthDays);
    void dayFailed(const QString &key, int year, int month, int day, const QString &error);
    // The camera can't be asked yet (its device not connected or its login not loaded): no
    // answer about the day; ask again later.
    void dayNotReady(const QString &key, int year, int month, int day);

private:
    Sources m_sources;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<QThreadPool> m_pool;
};

} // namespace rl
