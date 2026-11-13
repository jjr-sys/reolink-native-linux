#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace rl {

// Playback stills (wayfinder/fisheye-and-framing step 08): each pane asks for a picture at
// the playhead. At most `perRecorder` are fetched at once from one recorder (an NVR has a
// connection limit); the newest request per pane wins; playing cancels them all. `startStill`
// tells a pane to fetch; the pane reports `done` when its picture is up (or it gave up).
class StillQueue : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int perRecorder READ perRecorder WRITE setPerRecorder NOTIFY perRecorderChanged)

public:
    explicit StillQueue(QObject *parent = nullptr) : QObject(parent) { m_clock.start(); }

    int perRecorder() const { return m_perRecorder; }
    void setPerRecorder(int n);

    Q_INVOKABLE void request(const QString &pane, qint64 recorder);
    // `picture`: whether the still showed one (logged with how long it took).
    Q_INVOKABLE void done(const QString &pane, bool picture = true);
    Q_INVOKABLE void cancel(const QString &pane);
    Q_INVOKABLE void cancelAll();

    int running(qint64 recorder) const;
    int waiting() const { return int(m_waiting.size()); }

signals:
    void startStill(const QString &pane);
    void perRecorderChanged();

private:
    struct Entry
    {
        QString pane;
        qint64 recorder = 0;
    };
    void startNext(qint64 recorder);

    int m_perRecorder = 4;
    QHash<QString, qint64> m_running; // pane -> recorder
    QVector<Entry> m_waiting;         // in request order
    QHash<QString, qint64> m_startedMs; // pane -> when it last started (for the log)
    QElapsedTimer m_clock;
};

} // namespace rl
