#pragma once

#include "StreamPlayer.h"

#include <QVariantMap>
#include <functional>

namespace rl {
class DeviceManager;

// Internal seams of LiveStream. Production and scripted tests use the same lifecycle.
class LiveStreamTransport : public QObject
{
    Q_OBJECT
public:
    enum class Mode { Url, BaichuanMain, BaichuanSub };
    struct Description {
        bool exists = false;
        bool direct = false;
        QString url;
    };

    virtual void setDevices(DeviceManager *) {}
    virtual Description describe(const QVariantMap &camera, bool main) = 0;
    virtual void open(const QVariantMap &camera, bool main, Mode mode, quint64 attempt) = 0;
    virtual void stop() = 0;
    StreamPlayer player;

signals:
    void changed();
    void stateChanged(quint64 attempt, rl::StreamPlayer::State state, const QString &error);
};

class LiveStreamScheduler
{
public:
    enum class Task { Retry, Hide };
    virtual ~LiveStreamScheduler() = default;
    virtual void schedule(Task task, int delayMs, std::function<void()> callback) = 0;
    virtual void cancel(Task task) = 0;
};
} // namespace rl
