#pragma once

#include "protocol/BaichuanControl.h"

#include <QByteArray>
#include <QObject>

#include <memory>

namespace rl {

// Holds one Baichuan connection to a device, subscribed to the detections it pushes
// (cmd 31 → cmd 33, see BcAlarmEvents.h), and hands each pushed event list to the GUI
// thread. Runs a blocking loop on its own thread; reconnects with a 5–60 s backoff, and
// waits 10 minutes after a refused login so the firmware's lockout counter isn't burned.
// The loop follows the Outpost relay's NvrSource, which runs this at the household's NVRs.
class BaichuanEventListener : public QObject
{
    Q_OBJECT
public:
    explicit BaichuanEventListener(BaichuanControl::Params params, QObject *parent = nullptr);
    // Returns at once: the worker is told to stop and finishes on its own (a login to an
    // unreachable device can block for seconds), never touching this object again.
    ~BaichuanEventListener() override;

    // At app exit, after deleting the listeners: wait up to `ms` for their workers to end.
    static void waitForWorkers(int ms);

    const BaichuanControl::Params &params() const { return m_params; }
    bool subscribed() const { return m_subscribed; }

signals:
    // True once the device accepted the subscription; false when the connection is lost.
    void subscribedChanged(bool on);
    // A cmd 33 body (an <AlarmEventList>), for bc::parseAlarmEvents.
    void alarmEvents(const QByteArray &xml);
    // The device turned the login down (bad credentials or a locked account).
    void refused();

private:
    struct Shared; // what the worker holds: the stop flag and a pointer back, under a lock
    static void run(std::shared_ptr<Shared> shared, BaichuanControl::Params params);

    BaichuanControl::Params m_params;
    std::shared_ptr<Shared> m_shared;
    bool m_subscribed = false; // GUI thread only
};

} // namespace rl
