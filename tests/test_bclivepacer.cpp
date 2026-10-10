#include "protocol/BcLiveFramePacer.h"

#include <QtTest>

#include <algorithm>
#include <vector>

namespace {
constexpr qint64 kMs = 1000;
struct Arrival { qint64 atUs; quint32 ptsUs; bool keyFrame = false; };

qint64 worstGapMs(const std::vector<qint64> &shown)
{
    qint64 worst = 0;
    for (size_t i = 1; i < shown.size(); ++i)
        worst = std::max(worst, (shown[i] - shown[i - 1]) / kMs);
    return worst;
}

std::vector<qint64> replay(rl::BcLiveFramePacer &pacer, std::vector<Arrival> arrivals,
                           qint64 untilUs)
{
    std::stable_sort(arrivals.begin(), arrivals.end(),
                     [](const Arrival &a, const Arrival &b) { return a.atUs < b.atUs; });
    std::vector<qint64> shown;
    size_t next = 0;
    for (qint64 now = 0; now <= untilUs; now += kMs) {
        while (next < arrivals.size() && arrivals[next].atUs <= now) {
            const auto &a = arrivals[next++];
            pacer.push(QByteArray(16, 'v'), a.ptsUs, a.atUs, a.keyFrame);
        }
        QByteArray frame;
        while (pacer.popDue(now, frame))
            shown.push_back(now);
    }
    return shown;
}
} // namespace

class TestBcLivePacer : public QObject
{
    Q_OBJECT
private slots:
    void burstArrivalsAreReleasedAtTheCameraCadence()
    {
        rl::BcLiveFramePacer::Config cfg;
        cfg.delayUs = 100 * kMs;
        cfg.maxDepthUs = 1000 * kMs;
        rl::BcLiveFramePacer pacer(cfg);

        std::vector<Arrival> arrivals;
        for (int i = 0; i < 120; ++i) {
            // The source is steady at 25 fps, but the network releases four frames
            // together every 160 ms, matching the hitches reported on Pool HD.
            arrivals.push_back({(20 + (i / 4) * 160) * kMs,
                                quint32(i * 40 * kMs), i == 0});
        }

        std::vector<qint64> directDelivery;
        for (const Arrival &a : arrivals)
            directDelivery.push_back(a.atUs);
        QVERIFY(worstGapMs(directDelivery) > 100); // the unpaced path visibly hitches

        const auto shown = replay(pacer, arrivals, 10000 * kMs);
        QCOMPARE(int(shown.size()), 120);
        QVERIFY2(worstGapMs(shown) <= 41, qPrintable(QString::number(worstGapMs(shown))));
        QCOMPARE(pacer.stats().overflowed, qint64(0));
    }

    void aLateBurstMovesTheScheduleForwardWithoutDroppingFrames()
    {
        rl::BcLiveFramePacer::Config cfg;
        cfg.delayUs = 100 * kMs;
        cfg.lateToleranceUs = 50 * kMs;
        rl::BcLiveFramePacer pacer(cfg);
        std::vector<Arrival> arrivals;
        for (int i = 0; i < 100; ++i) {
            const qint64 at = i < 10 ? (20 + i * 40) * kMs : (2000 + (i - 10) * 40) * kMs;
            arrivals.push_back({at, quint32(i * 40 * kMs), i == 0 || i == 10});
        }
        const auto shown = replay(pacer, arrivals, 10000 * kMs);
        QCOMPARE(int(shown.size()), 100);
        QCOMPARE(pacer.stats().reanchors, 1);
        std::vector<qint64> after;
        for (qint64 t : shown)
            if (t > 3000 * kMs)
                after.push_back(t);
        QVERIFY(after.size() > 40);
        QVERIFY2(worstGapMs(after) <= 41, qPrintable(QString::number(worstGapMs(after))));
    }

    void theCameraClockCanWrapWithoutAReanchor()
    {
        rl::BcLiveFramePacer::Config cfg;
        cfg.delayUs = 100 * kMs;
        rl::BcLiveFramePacer pacer(cfg);
        std::vector<Arrival> arrivals;
        const quint32 first = 0xffff0000u;
        for (int i = 0; i < 80; ++i)
            arrivals.push_back({(20 + i * 40) * kMs,
                                quint32(first + quint32(i * 40 * kMs)), i == 0});
        const auto shown = replay(pacer, arrivals, 6000 * kMs);
        QCOMPARE(int(shown.size()), 80);
        QCOMPARE(pacer.stats().reanchors, 0);
        QVERIFY2(worstGapMs(shown) <= 41, qPrintable(QString::number(worstGapMs(shown))));
    }

    void overflowWaitsForAKeyFrame()
    {
        rl::BcLiveFramePacer::Config cfg;
        cfg.maxFrames = 2;
        cfg.maxBytes = 1024;
        rl::BcLiveFramePacer pacer(cfg);
        QVERIFY(pacer.push(QByteArray(400, 'i'), 1000, 0, true));
        QVERIFY(pacer.push(QByteArray(400, 'p'), 41000, 1000, false));
        QVERIFY(!pacer.push(QByteArray(400, 'p'), 81000, 2000, false));
        QVERIFY(!pacer.push(QByteArray(20, 'p'), 121000, 3000, false));
        QVERIFY(pacer.push(QByteArray(400, 'i'), 161000, 4000, true));
        QCOMPARE(pacer.size(), 1);
        QCOMPARE(pacer.queuedBytes(), qint64(400));
    }
};

QTEST_GUILESS_MAIN(TestBcLivePacer)
#include "test_bclivepacer.moc"
