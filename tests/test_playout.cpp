#include "media/VideoPlayoutQueue.h"

#include <QtTest>

#include <algorithm>
#include <random>
#include <vector>

// The choppy-video regression. Live frames arrive in bursts and stalls; VideoPlayoutQueue
// holds each until pts + delay so the picture keeps a steady cadence. These run on a
// virtual clock: `arrivals` are (arrival time, pts) pairs and the display polls every 5 ms.

namespace {
constexpr qint64 kMs = 1000;

struct Arrival { qint64 atUs; qint64 ptsUs; };
using Queue = rl::VideoPlayoutQueue<int>;

Queue::Config cfg(qint64 delayMs = 1000)
{
    Queue::Config c;
    c.delayUs = delayMs * kMs;
    c.maxDepthUs = 2500 * kMs;
    return c;
}

// Run the queue; returns the wall times (us) at which frames were presented.
std::vector<qint64> run(Queue &q, std::vector<Arrival> arrivals, qint64 untilUs)
{
    std::stable_sort(arrivals.begin(), arrivals.end(), [](const Arrival &a, const Arrival &b) { return a.atUs < b.atUs; });
    std::vector<qint64> shown;
    size_t next = 0;
    int payload = 0;
    for (qint64 t = 0; t <= untilUs; t += 5 * kMs) {
        while (next < arrivals.size() && arrivals[next].atUs <= t)
            q.push(payload++, arrivals[next].ptsUs, arrivals[next].atUs), ++next;
        int f;
        if (q.poll(t, f))
            shown.push_back(t);
    }
    return shown;
}

// Longest wait between consecutive presentations, in ms.
qint64 worstGapMs(const std::vector<qint64> &shown)
{
    qint64 worst = 0;
    for (size_t i = 1; i < shown.size(); ++i)
        worst = std::max(worst, (shown[i] - shown[i - 1]) / kMs);
    return worst;
}

// One frame every `stepMs` of camera time, `n` frames, arriving on the schedule `arrive`.
template <class F>
std::vector<Arrival> frames(int n, int stepMs, F arrive)
{
    std::vector<Arrival> a;
    for (int i = 0; i < n; ++i)
        a.push_back({arrive(i) * kMs, qint64(i) * stepMs * kMs});
    return a;
}
} // namespace

class TestPlayout : public QObject
{
    Q_OBJECT
private slots:
    void firstFrameWaitsForTheDelay()
    {
        Queue q(cfg());
        q.push(1, 0, 0);
        int f = 0;
        QVERIFY(!q.poll(999 * kMs, f));
        QCOMPARE(q.nextDueUs(), 1000 * kMs);
        QVERIFY(q.poll(1000 * kMs, f));
        QCOMPARE(f, 1);
    }

    // The camera is steady (10 fps) but the network delivers four frames at a time.
    void burstsAreShownAtASteadyCadence()
    {
        Queue q(cfg());
        auto a = frames(200, 100, [](int i) { return 100 + (i / 4) * 400; });
        const auto shown = run(q, a, 30000 * kMs);
        QCOMPARE(int(shown.size()), 200);
        QVERIFY2(worstGapMs(shown) <= 110, qPrintable(QString::number(worstGapMs(shown))));
        QCOMPARE(q.stats().droppedLate, qint64(0));
        QCOMPARE(q.stats().reanchors, 0);
    }

    void aStallShorterThanTheDelayIsInvisible()
    {
        Queue q(cfg());
        // Steady 10 fps, then a 700 ms hole in delivery every 3 s; frames due in it land together after.
        auto a = frames(200, 100, [](int i) {
            const int t = 100 + i * 100, ph = t % 3000;
            return (t >= 3000 && ph < 700) ? t - ph + 700 : t;
        });
        const auto shown = run(q, a, 30000 * kMs);
        QVERIFY2(worstGapMs(shown) <= 110, qPrintable(QString::number(worstGapMs(shown))));
        QCOMPARE(q.stats().reanchors, 0);
    }

    void aStallLongerThanTheDelayFreezesOnceThenRecovers()
    {
        Queue q(cfg());
        // A 2 s hole at t=5 s. The cushion (1 s) runs out, so the picture freezes once.
        auto a = frames(200, 100, [](int i) {
            const int t = 100 + i * 100;
            return (t >= 5000 && t < 7000) ? 7000 : t;
        });
        const auto shown = run(q, a, 30000 * kMs);
        QCOMPARE(q.stats().reanchors, 1);
        QVERIFY2(worstGapMs(shown) < 2500, qPrintable(QString::number(worstGapMs(shown))));
        // ...and after recovering it is steady again.
        std::vector<qint64> after;
        for (qint64 t : shown)
            if (t > 12000 * kMs)
                after.push_back(t);
        QVERIFY(after.size() > 50);
        QVERIFY(worstGapMs(after) <= 110);
    }

    void randomJitterKeepsACleanCadence()
    {
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> jit(0, 400);
        Queue q(cfg());
        std::vector<Arrival> a;
        qint64 lastAt = 0;
        for (int i = 0; i < 300; ++i) { // arrival = due time + up to 400 ms late, kept in order
            lastAt = std::max<qint64>(lastAt, (100 + i * 100 + jit(rng)) * kMs);
            a.push_back({lastAt, qint64(i) * 100 * kMs});
        }
        const auto shown = run(q, a, 40000 * kMs);
        QCOMPARE(q.stats().reanchors, 0);
        QVERIFY2(worstGapMs(shown) <= 110, qPrintable(QString::number(worstGapMs(shown))));
    }

    void timestampsGoingBackwardsStartAFreshSchedule()
    {
        Queue q(cfg());
        q.push(1, 5000 * kMs, 0);
        q.push(2, 5100 * kMs, 100 * kMs);
        q.push(3, 100 * kMs, 200 * kMs); // camera restarted its clock
        QCOMPARE(q.stats().reanchors, 1);
        QCOMPARE(q.size(), 1); // the old schedule's frames are gone
        QCOMPARE(q.nextDueUs(), 1200 * kMs);
    }

    void aSourceRunningFastDoesNotBuildUnboundedDelay()
    {
        Queue q(cfg());
        // Camera clock 10% fast: 100 ms of pts advance per 91 ms of wall time.
        std::vector<Arrival> a;
        for (int i = 0; i < 600; ++i)
            a.push_back({(100 + qint64(i) * 91) * kMs, qint64(i) * 100 * kMs});
        int maxSize = 0, f = 0, payload = 0;
        size_t next = 0;
        for (qint64 t = 0; t <= 60000 * kMs; t += 5 * kMs) {
            while (next < a.size() && a[next].atUs <= t)
                q.push(payload++, a[next].ptsUs, a[next].atUs), ++next;
            q.poll(t, f);
            maxSize = std::max(maxSize, q.size());
        }
        // Depth is capped near maxDepth (2.5 s of 10 fps frames), never runaway.
        QVERIFY2(maxSize <= 32, qPrintable(QString::number(maxSize)));
    }

    void aLateTimerShowsTheNewestDueFrameNotAFlurry()
    {
        Queue q(cfg());
        for (int i = 0; i < 5; ++i)
            q.push(i, qint64(i) * 100 * kMs, 0);
        int f = -1;
        QVERIFY(q.poll(1450 * kMs, f)); // frames 0..4 are due (due 1000..1400 ms)
        QCOMPARE(f, 4);
        QCOMPARE(q.stats().droppedLate, qint64(4));
        QCOMPARE(q.size(), 0);
    }

    void frameAndByteCapsBoundMemory()
    {
        Queue::Config c = cfg();
        c.maxFrames = 8;
        Queue q(c);
        for (int i = 0; i < 20; ++i)
            q.push(i, qint64(i) * 10 * kMs, qint64(i) * 10 * kMs);
        QVERIFY(q.size() <= 8);
        QVERIFY(q.stats().droppedOverflow >= 12);

        Queue::Config b = cfg();
        b.maxBytes = 5000;
        Queue qb(b);
        for (int i = 0; i < 10; ++i)
            qb.push(i, qint64(i) * 10 * kMs, qint64(i) * 10 * kMs, 1000);
        QVERIFY2(qb.queuedBytes() <= 5000, qPrintable(QString::number(qb.queuedBytes())));
    }

    void clearForgetsTheSchedule()
    {
        Queue q(cfg());
        q.push(1, 0, 0);
        q.clear();
        QCOMPARE(q.size(), 0);
        QCOMPARE(q.nextDueUs(), qint64(-1));
        q.push(2, 9000 * kMs, 50 * kMs); // a new stream anchors afresh, no "re-anchor"
        QCOMPARE(q.stats().reanchors, 0);
        QCOMPARE(q.nextDueUs(), 1050 * kMs);
    }
};

QTEST_GUILESS_MAIN(TestPlayout)
#include "test_playout.moc"
