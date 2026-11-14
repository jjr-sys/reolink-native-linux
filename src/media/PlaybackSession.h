#pragma once

#include "PlaybackTransport.h"

#include <QObject>
#include <QPointer>
#include <memory>

#include "StreamPlayer.h"
#include "device/DeviceManager.h"

namespace rl {

// One playback view's decisions: which transport carries the footage, whether a
// seek moves the running session or reopens, and when a change of quality/speed
// needs the stream reopened. QML supplies intent (row, hd, speed, "play this
// moment"); everything below that is here and tested through PlaybackTransport.
class PlaybackSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::DeviceManager *devices READ devices WRITE setDevices NOTIFY devicesChanged)
    Q_PROPERTY(rl::StreamPlayer *player READ player WRITE setPlayer NOTIFY playerChanged)
    Q_PROPERTY(int deviceRow READ deviceRow WRITE setDeviceRow NOTIFY deviceRowChanged)
    Q_PROPERTY(bool hd READ hd WRITE setHd NOTIFY hdChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(int direction READ direction WRITE setDirection NOTIFY directionChanged)
    Q_PROPERTY(rl::PlaybackSession::Route route READ route NOTIFY routeChanged)

public:
    enum class Route { Http, BaichuanMain, BaichuanSub, BaichuanReverse };
    Q_ENUM(Route)
    enum class Outcome { Gap, NoSource, Opened, OpenedHd, SeekedInPlace };
    Q_ENUM(Outcome)

    explicit PlaybackSession(QObject *parent = nullptr);
    // C++ only: scripted or alternative transport, never a QML setting.
    explicit PlaybackSession(std::unique_ptr<PlaybackTransport> transport,
                             QObject *parent = nullptr);
    ~PlaybackSession() override;

    // Pure policy. Direct (Frigate) footage is one HTTP stream at 1x: HD and speed
    // do not apply. A Reolink NVR always plays over native Baichuan: HD = main stream,
    // else sub, at any speed. (Its HTTP-FLV playback endpoint is not used: NVRs drop
    // every playback.bcs request (live FLV works), and it needs RTMP enabled anyway.)
    // Backwards (direction -1) plays windows of the sub stream (ticket 12).
    static Route chooseRoute(bool hd, double speed, bool direct, int direction = 1);
    Route route() const;

    DeviceManager *devices() const { return m_devices; }
    void setDevices(DeviceManager *devices);
    StreamPlayer *player() const { return m_player; }
    void setPlayer(StreamPlayer *player);
    int deviceRow() const { return m_row; }
    void setDeviceRow(int row);
    bool hd() const { return m_hd; }
    void setHd(bool hd);
    double speed() const { return m_speed; }
    void setSpeed(double speed);
    int direction() const { return m_direction; }
    void setDirection(int direction);

    // Play the wall-clock moment `epoch`. `covered` is the caller's timeline verdict
    // (a gap is normal: stop, no error). A native session is moved in place when one
    // is streaming, else (re)opened; HTTP always reopens at the snapped file.
    Q_INVOKABLE rl::PlaybackSession::Outcome playAt(double epoch, bool covered = true);
    // Always the light sub stream (event jump), whatever HD says: native Baichuan sub
    // for a Reolink device, the HTTP stream for direct (Frigate) footage.
    Q_INVOKABLE rl::PlaybackSession::Outcome playSubAt(double epoch);
    Q_INVOKABLE void stop();

signals:
    void devicesChanged();
    void playerChanged();
    void deviceRowChanged();
    void hdChanged();
    void speedChanged();
    void directionChanged();
    void routeChanged();
    // The transport for the current moment changed while playing: replay it.
    void reopenNeeded();

private:
    template <typename Change> void changeRouteAware(Change change);
    bool direct() const { return m_transport->direct(m_row); }
    Outcome openHttp(qint64 epoch);
    bool openOrSeekNative(qint64 epoch, bool main, bool reverse = false);

    Outcome playAtImpl(double epochSecs, bool covered);
    std::unique_ptr<PlaybackTransport> m_transport;
    class DeviceTransport *m_device = nullptr; // set when m_transport is the production one
    QPointer<DeviceManager> m_devices;
    QPointer<StreamPlayer> m_player;
    int m_row = -1;
    bool m_hd = false;
    double m_speed = 1.0;
    // The native stream this session last opened (main = HD), so a reopen after an HD/speed
    // change opens the other stream instead of seeking the running one in place.
    bool m_nativeOpen = false;
    bool m_openMain = false;
    bool m_openReverse = false;
    int m_direction = 1;
};

} // namespace rl
