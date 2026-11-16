#include "PlaybackSession.h"

#include "core/Log.h"

#include <QDateTime>

#include "StreamPlayer.h"
#include "device/DeviceManager.h"

namespace rl {

// Production adapter: DeviceManager opens streams into the view's StreamPlayer.
class DeviceTransport final : public PlaybackTransport
{
public:
    QPointer<DeviceManager> devices;
    QPointer<StreamPlayer> player;

    bool streaming() const override { return player && player->state() == StreamPlayer::State::Streaming; }
    bool busy() const override { return player && player->state() == StreamPlayer::State::Connecting; }
    bool direct(int row) const override { return devices && row >= 0 && devices->isDirectAt(row); }
    QString httpUrl(int row, qint64 epoch) override
    {
        return devices ? devices->playbackUrl(row, epoch) : QString();
    }
    // This pane's own stream (each pane's session has its own player).
    bool seekInPlace(int, qint64 epoch) override { return player && player->seekSource(epoch); }
    void openHttp(int row, qint64, const QString &url) override
    {
        if (!devices || !player)
            return;
        player->setLoop(false);
        player->setReversePlayback(false);
        player->setForwardWindows(false);
        player->setExpectedSize(devices->declaredSize(row, false));
        player->setSource(url);
        player->start();
    }
    void openBaichuan(int row, qint64 epoch, bool main, NativeWindows windows) override
    {
        if (!devices || !player)
            return;
        player->setLoop(false);
        devices->startBaichuanPlayback(row, epoch, player, main, windows == NativeWindows::Backward,
                                       windows == NativeWindows::Forward);
    }
    void stop() override
    {
        if (player)
            player->stop();
    }
    bool bufferHas(int row, qint64 epochMs, int direction) const override
    {
        return devices && devices->liveBufferHas(row, epochMs, direction);
    }
    void openBuffer(int row, qint64 epochMs, int direction) override
    {
        if (!devices || !player)
            return;
        player->setLoop(false);
        devices->startLiveBufferPlayback(row, epochMs, direction, player);
    }
};

namespace {
// Stand-in until devices/player are bound: nothing plays.
class NullTransport final : public PlaybackTransport
{
public:
    bool streaming() const override { return false; }
    bool busy() const override { return false; }
    bool direct(int) const override { return false; }
    QString httpUrl(int, qint64) override { return {}; }
    bool seekInPlace(int, qint64) override { return false; }
    void openHttp(int, qint64, const QString &) override {}
    void openBaichuan(int, qint64, bool, NativeWindows) override {}
    void stop() override {}
};
} // namespace

PlaybackSession::PlaybackSession(QObject *parent)
    : QObject(parent)
{
    auto t = std::make_unique<DeviceTransport>();
    m_device = t.get();
    m_transport = std::move(t);
}

PlaybackSession::PlaybackSession(std::unique_ptr<PlaybackTransport> transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport ? std::move(transport) : std::make_unique<NullTransport>())
{
}

PlaybackSession::~PlaybackSession() = default;

PlaybackSession::Route PlaybackSession::chooseRoute(bool hd, double speed, bool direct, int direction)
{
    if (direct)
        return Route::Http;
    const bool hdHolds = hd && speed <= kHdTopSpeed;
    if (direction < 0)
        return hdHolds ? Route::BaichuanReverseMain : Route::BaichuanReverse;
    if (hdHolds)
        return speed <= 1.0 ? Route::BaichuanMain : Route::BaichuanForwardMain;
    return Route::BaichuanSub;
}

void PlaybackSession::updateHdHeldBack()
{
    const bool held = route() != chooseRoute(m_hd, m_speed, direct(), m_direction);
    if (held == m_hdHeldBack)
        return;
    m_hdHeldBack = held;
    emit hdHeldBackChanged();
}

PlaybackSession::Route PlaybackSession::route() const
{
    const Route r = chooseRoute(m_hd, m_speed, direct(), m_direction);
    // Too slow for HD in windows: SD where windows would carry HD (recorder-paced 1× stays HD).
    if (m_linkSlow && (r == Route::BaichuanReverseMain || r == Route::BaichuanForwardMain))
        return chooseRoute(false, m_speed, direct(), m_direction);
    return r;
}

void PlaybackSession::setDevices(DeviceManager *devices)
{
    if (m_devices == devices)
        return;
    m_devices = devices;
    if (m_device)
        m_device->devices = devices;
    emit devicesChanged();
}

void PlaybackSession::setPlayer(StreamPlayer *player)
{
    if (m_player == player)
        return;
    m_player = player;
    if (m_device)
        m_device->player = player;
    emit playerChanged();
}

void PlaybackSession::setDeviceRow(int row)
{
    if (m_row == row)
        return;
    m_row = row;
    m_nativeOpen = false; // another camera: nothing of this row is open
    emit deviceRowChanged();
    emit routeChanged(); // direct-ness is per row
    updateHdHeldBack();
}

// Apply a change to hd/speed; if the route moved while a stream is up or opening,
// ask the owner to replay the current moment on the new transport.
template <typename Change> void PlaybackSession::changeRouteAware(Change change)
{
    const Route before = route();
    change();
    updateHdHeldBack();
    if (route() == before)
        return;
    emit routeChanged();
    if (m_transport->streaming() || m_transport->busy())
        emit reopenNeeded();
}

void PlaybackSession::setHd(bool hd)
{
    if (m_hd == hd)
        return;
    changeRouteAware([&] { m_hd = hd; emit hdChanged(); });
}

void PlaybackSession::setDirection(int direction)
{
    direction = direction < 0 ? -1 : 1;
    if (m_direction == direction)
        return;
    changeRouteAware([&] {
        m_direction = direction;
        emit directionChanged();
        clearLinkSlow(); // a change of direction is an open the user makes: HD is tried again
    });
}

void PlaybackSession::setUseLiveBuffer(bool on)
{
    if (m_useLiveBuffer == on)
        return;
    m_useLiveBuffer = on;
    emit useLiveBufferChanged();
}

void PlaybackSession::clearLinkSlow()
{
    if (!m_linkSlow)
        return;
    m_linkSlow = false;
    emit linkSlowChanged();
    updateHdHeldBack();
}

void PlaybackSession::setLinkSlow(bool slow)
{
    if (m_linkSlow == slow)
        return;
    changeRouteAware([&] { m_linkSlow = slow; emit linkSlowChanged(); });
}

void PlaybackSession::setSpeed(double speed)
{
    if (qFuzzyCompare(m_speed, speed))
        return;
    changeRouteAware([&] { m_speed = speed; emit speedChanged(); });
}

PlaybackSession::Outcome PlaybackSession::openHttp(qint64 epoch)
{
    const QString url = m_transport->httpUrl(m_row, epoch);
    if (url.isEmpty())
        return Outcome::NoSource;
    m_transport->openHttp(m_row, epoch, url);
    return Outcome::Opened;
}

PlaybackSession::Outcome PlaybackSession::playAt(double epochSecs, bool covered)
{
    const Outcome o = playAtImpl(epochSecs, covered);
    const char *name = "?";
    switch (o) {
    case Outcome::Gap: name = "gap: stopped"; break;
    case Outcome::NoSource: name = "no source"; break;
    case Outcome::Opened: name = "opened"; break;
    case Outcome::OpenedHd: name = "opened HD"; break;
    case Outcome::SeekedInPlace: name = "seeked in place"; break;
    }
    qCInfo(lcMedia).noquote() << "playback row" << m_row << "at"
                              << QDateTime::fromSecsSinceEpoch(qint64(epochSecs)).toString(Qt::ISODate)
                              << (covered ? "" : "(timeline: no recording)") << "->"
                              << name;
    return o;
}

PlaybackSession::Outcome PlaybackSession::playAtImpl(double epochSecs, bool covered)
{
    if (!covered) {
        stop();
        return Outcome::Gap;
    }
    const qint64 epoch = static_cast<qint64>(epochSecs);
    const qint64 epochMs = static_cast<qint64>(epochSecs * 1000);
    if (m_useLiveBuffer && m_row >= 0 && m_transport->bufferHas(m_row, epochMs, m_direction)) {
        m_nativeOpen = false;   // the next recorder stream opens afresh
        m_transport->openBuffer(m_row, epochMs, m_direction);
        return Outcome::Opened;
    }
    const Route r = route();
    if (r == Route::Http) {
        m_nativeOpen = false;
        return openHttp(epoch);
    }
    if (m_row < 0)
        return Outcome::NoSource;
    bool main = false;
    NativeWindows windows = NativeWindows::None;
    switch (r) {
    case Route::BaichuanMain: main = true; break;
    case Route::BaichuanReverseMain: main = true; windows = NativeWindows::Backward; break;
    case Route::BaichuanReverse: windows = NativeWindows::Backward; break;
    case Route::BaichuanForwardMain: main = true; windows = NativeWindows::Forward; break;
    case Route::BaichuanSub:
    case Route::Http: break;
    }
    return openOrSeekNative(epoch, main, windows)
        ? Outcome::SeekedInPlace
        : main ? Outcome::OpenedHd : Outcome::Opened;
}

// Move the running native session in place (no reconnect) when it is the stream wanted;
// otherwise open that stream. True when it seeked.
bool PlaybackSession::openOrSeekNative(qint64 epoch, bool main, NativeWindows windows)
{
    // A run in windows has them fetched around the old moment: always reopened.
    if (windows == NativeWindows::None && m_nativeOpen && m_openMain == main
        && m_openWindows == NativeWindows::None
        && m_transport->streaming() && m_transport->seekInPlace(m_row, epoch))
        return true;
    m_transport->openBaichuan(m_row, epoch, main, windows);
    m_nativeOpen = true;
    m_openMain = main;
    m_openWindows = windows;
    return false;
}

PlaybackSession::Outcome PlaybackSession::playSubAt(double epochSecs)
{
    const qint64 epoch = static_cast<qint64>(epochSecs);
    if (direct()) {
        m_nativeOpen = false;
        return openHttp(epoch);
    }
    if (m_row < 0)
        return Outcome::NoSource;
    return openOrSeekNative(epoch, /*main=*/false) ? Outcome::SeekedInPlace : Outcome::Opened;
}

void PlaybackSession::stop()
{
    m_nativeOpen = false;
    m_transport->stop();
}

} // namespace rl
