#include "LiveStream.h"

#include <QScopedValueRollback>

namespace rl {
namespace {
using Mode = LiveStreamTransport::Mode;
using State = StreamPlayer::State;
using Task = LiveStreamScheduler::Task;

class QtLiveStreamScheduler final : public LiveStreamScheduler
{
public:
    void schedule(Task task, int delayMs, std::function<void()> callback) override
    {
        QTimer &timer = task == Task::Retry ? m_retry : m_hide;
        timer.stop();
        QObject::disconnect(&timer, nullptr, nullptr, nullptr);
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &timer, std::move(callback));
        timer.start(delayMs);
    }
    void cancel(Task task) override { (task == Task::Retry ? m_retry : m_hide).stop(); }
private:
    QTimer m_retry;
    QTimer m_hide;
};

class DeviceLiveStreamTransport final : public LiveStreamTransport
{
public:
    DeviceLiveStreamTransport()
    {
        player.setSmoothing(true);
        connect(&player, &StreamPlayer::stateChanged, this, [this] {
            emit stateChanged(m_attempt, player.state(), player.errorString());
        });
    }

    void setDevices(DeviceManager *devices) override
    {
        if (m_devices)
            disconnect(m_devices, nullptr, this, nullptr);
        m_devices = devices;
        if (devices) {
            // Defer structural updates until a remove+insert validation transaction
            // has completed. Resolve the exact camera again before any stream open.
            connect(devices, &QAbstractItemModel::rowsRemoved, this,
                    [this] { emit changed(); }, Qt::QueuedConnection);
            connect(devices, &QAbstractItemModel::rowsInserted, this,
                    [this] { emit changed(); }, Qt::QueuedConnection);
            connect(devices, &QAbstractItemModel::modelReset, this,
                    [this] { emit changed(); }, Qt::QueuedConnection);
            connect(devices, &QAbstractItemModel::dataChanged, this,
                    [this] { emit changed(); }, Qt::QueuedConnection);
            connect(devices, &QObject::destroyed, this, [this] { emit changed(); });
        }
        emit changed();
    }

    Description describe(const QVariantMap &camera, bool main) override
    {
        const int row = exactRow(camera);
        if (row < 0)
            return {};
        return {true, m_devices->isDirectAt(row), m_devices->liveUrl(row, main)};
    }

    void open(const QVariantMap &camera, bool main, Mode mode, quint64 attempt) override
    {
        m_attempt = attempt;
        const int row = exactRow(camera);
        if (row < 0)
            return;
        if (mode != Mode::Url) {
            player.setLoop(false);
            m_devices->startBaichuanLive(row, &player, mode == Mode::BaichuanMain);
        } else {
            player.setLoop(false); // live URLs recover; finite direct files finish once
            player.setExpectedSize(m_devices->declaredSize(row, main));
            player.setSource(m_devices->liveUrl(row, main));
            player.start();
        }
    }

    void stop() override { player.stop(); }

private:
    int exactRow(const QVariantMap &camera) const
    {
        if (!m_devices)
            return -1;
        for (int row = 0; row < m_devices->rowCount(); ++row) {
            const QModelIndex index = m_devices->index(row);
            if (m_devices->data(index, DeviceManager::HostIdRole).toLongLong()
                    == camera.value("hostId").toLongLong()
                && m_devices->data(index, DeviceManager::ChannelRole).toInt()
                    == camera.value("channel").toInt())
                return row;
        }
        return -1;
    }
    QPointer<DeviceManager> m_devices;
    quint64 m_attempt = 0;
};

bool terminalMediaError(const QString &error)
{
    return error == QLatin1String("no video stream")
           || error == QLatin1String("unsupported codec")
           || error == QLatin1String("could not open decoder");
}
} // namespace

LiveStream::LiveStream(QObject *parent)
    : LiveStream(std::make_unique<DeviceLiveStreamTransport>(),
                 std::make_unique<QtLiveStreamScheduler>(), parent) {}

LiveStream::LiveStream(std::unique_ptr<LiveStreamTransport> transport,
                       std::unique_ptr<LiveStreamScheduler> scheduler, QObject *parent)
    : QObject(parent), m_transport(std::move(transport)), m_scheduler(std::move(scheduler))
{
    connect(m_transport.get(), &LiveStreamTransport::changed, this, &LiveStream::refresh);
    connect(m_transport.get(), &LiveStreamTransport::stateChanged, this, &LiveStream::onState);
    auto *player = &m_transport->player;
    connect(player, &StreamPlayer::videoSinkChanged, this, &LiveStream::videoSinkChanged);
    connect(player, &StreamPlayer::mutedChanged, this, &LiveStream::mutedChanged);
    connect(player, &StreamPlayer::hasAudioChanged, this, &LiveStream::hasAudioChanged);
    connect(player, &StreamPlayer::recordingChanged, this, &LiveStream::recordingChanged);
    connect(player, &StreamPlayer::recordingSaved, this, &LiveStream::recordingSaved);
    connect(player, &StreamPlayer::recordingFailed, this, &LiveStream::recordingFailed);
}

LiveStream::~LiveStream() { release(); }

void LiveStream::setDevices(DeviceManager *devices)
{
    if (m_devices == devices)
        return;
    stopConnection();
    m_devices = devices;
    m_transport->setDevices(devices);
    emit devicesChanged();
    refresh();
}

void LiveStream::setCamera(const QVariantMap &camera)
{
    if (m_camera == camera)
        return;
    stopConnection();
    resetPolicy();
    m_camera = camera;
    m_released = false;
    updateAudio();
    emit cameraChanged();
    refresh();
}

void LiveStream::setMainStream(bool main)
{
    if (m_main == main)
        return;
    if (m_visible || !m_open)
        stopConnection();
    resetPolicy();
    m_main = main;
    emit mainStreamChanged();
    refresh();
}

void LiveStream::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (active) {
        m_released = false;
        resetPolicy();
    } else {
        stopConnection();
    }
    updateAudio();
    emit activeChanged();
    refresh();
}

void LiveStream::setViewVisible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    ++m_hideEpoch;
    m_scheduler->cancel(Task::Hide);
    m_hidePending = false;
    updateAudio();
    if (visible) {
        if (!m_open)
            resetPolicy();
    } else {
        m_scheduler->cancel(Task::Retry);
        m_retryPending = false;
        setRecovering(false);
        if (m_open && m_active) {
            m_hidePending = true;
            const quint64 epoch = m_hideEpoch;
            m_scheduler->schedule(Task::Hide, 5000, [this, epoch] {
                if (epoch == m_hideEpoch && m_hidePending && !m_visible)
                    stopConnection();
            });
        }
    }
    emit viewVisibleChanged();
    refresh();
}

void LiveStream::setMuted(bool muted)
{
    m_muted = muted;
    updateAudio();
}

void LiveStream::updateAudio()
{
    m_transport->player.setMuted(m_muted || !m_visible || !m_active || m_released);
}

void LiveStream::release()
{
    m_released = true;
    stopConnection();
    updateAudio();
}

void LiveStream::stopConnection()
{
    QScopedValueRollback changing(m_changing, true);
    ++m_attempt;
    ++m_hideEpoch;
    m_scheduler->cancel(Task::Retry);
    m_scheduler->cancel(Task::Hide);
    m_retryPending = false;
    m_hidePending = false;
    m_open = false;
    m_key.clear();
    m_transport->stop();
    setRecovering(false);
    setState(State::Stopped);
}

void LiveStream::resetPolicy()
{
    m_bcSub = false;
    m_terminal = false;
    m_retries = 0;
    m_backoffMs = 1000;
    setDegraded(false);
}

void LiveStream::refresh()
{
    if (m_changing || !m_complete)
        return;
    if (!m_active || m_released || m_camera.value("hostId").toLongLong() <= 0
        || !m_camera.contains("channel") || m_camera.value("channel").toInt() < 0) {
        if (m_open || m_retryPending || m_hidePending)
            stopConnection();
        return;
    }
    // Hiding is managed by one fixed deadline. Model changes must not prolong it
    // or open new connections in a tile that is no longer on screen.
    const auto source = m_transport->describe(m_camera, m_main);
    if (!source.exists || (m_visible && source.url.isEmpty())) {
        stopConnection();
        setState(source.exists ? State::Connecting : State::Stopped,
                 source.exists ? tr("Waiting for camera") : tr("Camera unavailable"));
        return;
    }
    if (!m_visible)
        return;
    const Mode mode = source.direct ? Mode::Url
                     : m_main && !m_degraded ? Mode::BaichuanMain
                     : !m_main && m_bcSub ? Mode::BaichuanSub : Mode::Url;
    const QString key = QString::number(int(mode)) + u'/' + source.url;
    if (m_retryPending || ((m_open || m_terminal) && key == m_key))
        return;

    QScopedValueRollback changing(m_changing, true);
    ++m_attempt;
    m_transport->stop();
    m_open = true;
    m_terminal = false;
    m_mode = mode;
    m_key = key;
    setState(State::Connecting);
    m_transport->open(m_camera, m_main, mode, m_attempt);
}

void LiveStream::retry(int delayMs)
{
    m_retryPending = true;
    setRecovering(true);
    const quint64 attempt = m_attempt;
    m_scheduler->schedule(Task::Retry, delayMs, [this, attempt] {
        if (attempt != m_attempt || !m_retryPending)
            return;
        m_retryPending = false;
        refresh();
    });
}

void LiveStream::onState(quint64 attempt, State state, const QString &error)
{
    if (attempt != m_attempt || !m_open || (m_changing && state == State::Stopped))
        return;
    if (state == State::Streaming) {
        m_retries = 0;
        m_backoffMs = 1000;
        setRecovering(false);
        setState(state);
        return;
    }
    if (state != State::Stopped && state != State::Error) {
        setState(state, error);
        return;
    }

    const auto source = m_transport->describe(m_camera, m_main);
    if (m_mode == Mode::Url) {
        if (state == State::Error && !source.direct && !m_main && !m_bcSub
            && error.startsWith(QLatin1String("Stream not found"))) {
            stopConnection();
            m_bcSub = true;
            refresh();
            return;
        }
        // URL reconnect/backoff is internal to StreamPlayer. Do not add a second
        // loop or retry terminal media errors and finite direct files.
        if (state == State::Stopped) {
            m_open = false;
            m_terminal = true;
        }
        setRecovering(state == State::Error && !terminalMediaError(error));
        setState(state, error.isEmpty() ? m_error : error);
        return;
    }

    const Mode failedMode = m_mode;
    stopConnection(); // invalidates the following Stopped event from this attempt
    if (!m_visible || !m_active || m_released)
        return;
    if (terminalMediaError(error)) {
        m_terminal = true;
        m_key = QString::number(int(failedMode)) + u'/' + source.url;
        setState(State::Error, error);
        return;
    }
    if (failedMode == Mode::BaichuanMain && m_retries >= 3) {
        setDegraded(true);
        refresh();
        return;
    }
    setState(State::Error, error.isEmpty() ? tr("Live stream interrupted") : error);
    if (failedMode == Mode::BaichuanMain) {
        ++m_retries;
        retry(1500);
    } else {
        retry(m_backoffMs);
        m_backoffMs = qMin(m_backoffMs * 2, 30000);
    }
}

void LiveStream::setState(State state, const QString &error)
{
    if (m_state == state && m_error == error)
        return;
    m_state = state;
    m_error = error;
    emit stateChanged();
}

void LiveStream::setDegraded(bool degraded)
{
    if (m_degraded == degraded)
        return;
    m_degraded = degraded;
    emit degradedChanged();
}

void LiveStream::setRecovering(bool recovering)
{
    if (m_recovering == recovering)
        return;
    m_recovering = recovering;
    emit recoveringChanged();
}
} // namespace rl
