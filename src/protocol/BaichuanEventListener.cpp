#include "BaichuanEventListener.h"

#include "protocol/BcAlarmEvents.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

namespace rl {

namespace {
constexpr int kRetryMs = 5'000;
constexpr int kMaxRetryMs = 60'000;
constexpr int kRefusedMs = 600'000;
constexpr qint64 kKeepaliveMs = 30'000;
constexpr int kReadWaitMs = 300; // how quickly a stop request is noticed while connected
std::atomic<int> g_workers{0};
} // namespace

void BaichuanEventListener::waitForWorkers(int ms)
{
    QDeadlineTimer until(ms);
    while (g_workers > 0 && !until.hasExpired())
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

struct BaichuanEventListener::Shared {
    std::atomic<bool> stop{false};
    std::mutex lock;
    BaichuanEventListener *owner = nullptr; // null once the listener is gone

    // Run `f` on the GUI thread against the owner, if it still exists.
    void post(std::function<void(BaichuanEventListener *)> f)
    {
        std::lock_guard<std::mutex> g(lock);
        if (BaichuanEventListener *o = owner)
            QMetaObject::invokeMethod(o, [o, f] { f(o); }, Qt::QueuedConnection);
    }
    bool sleepFor(int ms) // false if asked to stop meanwhile
    {
        QDeadlineTimer until(ms);
        while (!until.hasExpired()) {
            if (stop)
                return false;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min<qint64>(200, until.remainingTime())));
        }
        return !stop;
    }
};

BaichuanEventListener::BaichuanEventListener(BaichuanControl::Params params, QObject *parent)
    : QObject(parent), m_params(std::move(params)), m_shared(std::make_shared<Shared>())
{
    m_shared->owner = this;
    ++g_workers;
    std::thread(&BaichuanEventListener::run, m_shared, m_params).detach();
}

BaichuanEventListener::~BaichuanEventListener()
{
    // Queued calls already posted to this object are dropped by Qt when it is deleted.
    std::lock_guard<std::mutex> g(m_shared->lock);
    m_shared->owner = nullptr;
    m_shared->stop = true;
}

void BaichuanEventListener::run(std::shared_ptr<Shared> shared, BaichuanControl::Params params)
{
    struct Done { ~Done() { --g_workers; } } done;
    auto setSubscribed = [&shared](bool on) {
        shared->post([on](BaichuanEventListener *o) {
            if (on == o->m_subscribed)
                return;
            o->m_subscribed = on;
            emit o->subscribedChanged(on);
        });
    };
    int wait = kRetryMs;
    while (!shared->stop) {
        BaichuanControl control(params);
        if (control.open() && control.post(bc::kCmdSubscribeEvents)) {
            wait = kRetryMs;
            QElapsedTimer sinceKeepalive;
            sinceKeepalive.start();
            while (!shared->stop) {
                if (sinceKeepalive.elapsed() >= kKeepaliveMs) {
                    if (!control.post(bc::kCmdKeepalive))
                        break;
                    sinceKeepalive.restart();
                }
                bool lost = false;
                const auto msg = control.readPushed(kReadWaitMs, &lost);
                if (lost)
                    break;
                if (!msg)
                    continue;
                if (msg->cmdId == bc::kCmdSubscribeEvents) {
                    if (msg->status != 200)
                        break; // the device won't push to this connection
                    setSubscribed(true);
                } else if (msg->cmdId == bc::kCmdAlarmEvents) {
                    setSubscribed(true);
                    const QByteArray xml = msg->xml;
                    shared->post([xml](BaichuanEventListener *o) { emit o->alarmEvents(xml); });
                }
            }
            setSubscribed(false);
        } else if (control.refused()) {
            wait = kRefusedMs;
            shared->post([](BaichuanEventListener *o) { emit o->refused(); });
        }
        control.close();
        if (!shared->sleepFor(wait))
            return;
        if (wait < kRefusedMs)
            wait = std::min(wait * 2, kMaxRetryMs);
    }
}

} // namespace rl
