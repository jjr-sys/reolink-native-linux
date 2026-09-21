#pragma once

#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>

namespace rl {

// Copy the streams of `src` (any file libavformat opens) into a plain MP4 at `dst` with a
// real duration and seek index. No re-encoding. Returns the clip length in ms, or -1 with
// `error` set. Frigate cuts its clips on request as fragmented MP4 with a zero duration
// and no index, which a player can neither measure nor seek.
qint64 remuxToIndexedMp4(const QString &src, const QString &dst, QString *error = nullptr);

// Fetches a Frigate clip to a temporary file and makes it seekable, so the clip dialog
// can play it with a scrub bar. The server sends the clip chunked (no length, no ranges),
// so playing the URL directly cannot seek. Clips are a few MB.
class ClipLoader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QUrl localUrl READ localUrl NOTIFY stateChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)

public:
    enum class State { Idle, Loading, Ready, Error };
    Q_ENUM(State)

    explicit ClipLoader(QObject *parent = nullptr);
    ~ClipLoader() override;

    State state() const { return m_state; }
    QUrl localUrl() const { return m_localUrl; }
    qint64 durationMs() const { return m_durationMs; }
    QString errorString() const { return m_error; }

    Q_INVOKABLE void load(const QString &url);
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();

private:
    void setState(State s);

    QTemporaryDir m_dir;
    State m_state = State::Idle;
    QUrl m_localUrl;
    qint64 m_durationMs = 0;
    QString m_error;
    int m_generation = 0; // a finished download for an older load() is dropped
};

} // namespace rl
