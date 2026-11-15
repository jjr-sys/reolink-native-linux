#pragma once

#include <QObject>
#include <QPointer>
#include <functional>

namespace rl {

class DeviceManager;

// The Events page's clip player (wayfinder/events-feed step 22): which footage an event
// plays and over what window. A Reolink recorder plays its sub stream from kPreSecs before
// the event; a camera whose footage is on Frigate plays Frigate's own clip from
// kFrigatePreSecs before (Frigate's events reach the app up to 10 s late). Both run to
// kPostSecs after, where the player stops with Keep playing / Open in Playback.
// Positions are wall-clock epoch seconds.
class EventClip : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::DeviceManager *devices READ devices WRITE setDevices NOTIFY devicesChanged)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(qint64 hostId READ hostId NOTIFY changed)
    Q_PROPERTY(int channel READ channel NOTIFY changed)
    Q_PROPERTY(qint64 timestamp READ timestamp NOTIFY changed)
    Q_PROPERTY(bool frigate READ frigate NOTIFY changed)
    Q_PROPERTY(QString clipUrl READ clipUrl NOTIFY changed)
    Q_PROPERTY(qint64 from READ from NOTIFY changed)
    Q_PROPERTY(qint64 to READ to NOTIFY changed)
    Q_PROPERTY(bool openEnded READ openEnded NOTIFY changed)

public:
    static constexpr int kPreSecs = 5;
    static constexpr int kFrigatePreSecs = 15;
    static constexpr int kPostSecs = 30;
    static constexpr int kFrigateMoreSecs = 60; // Keep playing on Frigate: the next minute
    // (host, channel, timestamp, preSecs, postSecs) → a Frigate clip URL, "" for a recorder.
    using ClipResolver = std::function<QString(qint64, int, qint64, int, int)>;

    explicit EventClip(QObject *parent = nullptr);

    DeviceManager *devices() const;
    void setDevices(DeviceManager *devices);
    // Tests: stands in for DeviceManager::eventClipUrl.
    void setClipResolver(ClipResolver resolver);

    bool active() const { return m_active; }
    qint64 hostId() const { return m_host; }
    int channel() const { return m_channel; }
    qint64 timestamp() const { return m_ts; }
    bool frigate() const { return !m_url.isEmpty(); }
    QString clipUrl() const { return m_url; }
    qint64 from() const { return m_from; }
    qint64 to() const { return m_to; }
    bool openEnded() const { return m_openEnded; }

    Q_INVOKABLE void setEvent(qint64 hostId, int channel, qint64 timestamp);
    Q_INVOKABLE void clear();
    // Keep playing past `to`, with no end (openEnded): a recorder's stream plays on; a
    // Frigate clip can't, so each call moves the window to the next kFrigateMoreSecs as a
    // new clip (the player calls it again as each one ends).
    Q_INVOKABLE void keepPlaying();
    // Where `position` sits in [from, to], 0..1; and whether it has reached `to`.
    Q_INVOKABLE double progress(double position) const;
    Q_INVOKABLE bool atEnd(double position) const;
    // The pane bar (live-replay ticket 11): the moment at `fraction` (0..1) of the window, and
    // `secs` on from `position` kept inside it (no end once open-ended). 0 with no event.
    Q_INVOKABLE double timeAt(double fraction) const;
    Q_INVOKABLE double skipFrom(double position, double secs) const;
    // Ticket 12: whether playing `direction` (1 / -1) from `position` has reached the window's
    // edge — its start backwards (within a quarter second), its end forwards (atEnd).
    Q_INVOKABLE bool edgeReached(double position, int direction) const;
    // A recorder stream going `direction` stopped at `position`: where to open it again (a
    // second backwards from the window's start / forwards from its end is the edge), or -1 —
    // the edge, or it already reopened there (`lastReopenAt`, -1 for none) and ended again.
    Q_INVOKABLE double reopenAfterEnd(double position, int direction, double lastReopenAt) const;

signals:
    void devicesChanged();
    void changed();

private:
    QString resolve(qint64 hostId, int channel, qint64 timestamp, int pre, int post) const;

    QPointer<DeviceManager> m_devices;
    ClipResolver m_resolver;
    bool m_active = false;
    qint64 m_host = -1;
    int m_channel = 0;
    qint64 m_ts = 0;
    QString m_url;
    qint64 m_from = 0, m_to = 0;
    bool m_openEnded = false;
};

} // namespace rl
