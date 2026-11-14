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
        return devices ? devices->playbackUrl(row, epoch, false) : QString();
    }
    bool seekInPlace(int row, qint64 epoch) override
    {
        return devices && devices->seekBaichuanPlayback(row, epoch);
    }
    void openHttp(int row, qint64, const QString &url) override
    {
        if (!devices || !player)
            return;
        player->setLoop(false);
        player->setExpectedSize(devices->declaredSize(row, false));
        player->setSource(url);
        player->start();
    }
    void openBaichuan(int row, qint64 epoch, bool main) override
    {
        if (!devices || !player)
            return;
        player->setLoop(false);
        devices->startBaichuanPlayback(row, epoch, player, main);
    }
    void stop() override
    {
        if (player)
            player->stop();
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
    void openBaichuan(int, qint64, bool) override {}
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

PlaybackSession::Route PlaybackSession::chooseRoute(bool hd, double speed, bool direct)
{
    if (direct)
        return Route::Http;
    // Above 1x the recorder sends the main stream as keyframes only (wayfinder/playback-controls
    // ticket 10), so HD plays the sub stream at speed and comes back at 1x.
    return hd && speed <= 1.0 ? Route::BaichuanMain : Route::BaichuanSub;
}

PlaybackSession::Route PlaybackSession::route() const
{
    return chooseRoute(m_hd, m_speed, direct());
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
}

// Apply a change to hd/speed; if the route moved while a stream is up or opening,
// ask the owner to replay the current moment on the new transport.
template <typename Change> void PlaybackSession::changeRouteAware(Change change)
{
    const Route before = route();
    change();
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
    const Route r = route();
    if (r == Route::Http) {
        m_nativeOpen = false;
        return openHttp(epoch);
    }
    if (m_row < 0)
        return Outcome::NoSource;
    const bool main = r == Route::BaichuanMain;
    return openOrSeekNative(epoch, main) ? Outcome::SeekedInPlace
                                         : main ? Outcome::OpenedHd : Outcome::Opened;
}

// Move the running native session in place (no reconnect) when it is the stream wanted;
// otherwise open that stream. True when it seeked.
bool PlaybackSession::openOrSeekNative(qint64 epoch, bool main)
{
    if (m_nativeOpen && m_openMain == main && m_transport->streaming()
        && m_transport->seekInPlace(m_row, epoch))
        return true;
    m_transport->openBaichuan(m_row, epoch, main);
    m_nativeOpen = true;
    m_openMain = main;
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
