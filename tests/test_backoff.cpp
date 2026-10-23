#include "core/Backoff.h"

#include <QtTest>
#include <vector>

using namespace rl;

namespace {
// Delays a loop would wait if every attempt failed, n times in a row.
std::vector<int> failures(Backoff &b, int n)
{
    std::vector<int> out;
    for (int i = 0; i < n; ++i)
        out.push_back(b.fail());
    return out;
}
} // namespace

// Each row is a call site's policy plus the exact delay sequence its hand-rolled
// loop produced before the module existed (recorded from the old code).
class BackoffTest : public QObject
{
    Q_OBJECT
private slots:
    void sequences_data()
    {
        QTest::addColumn<int>("start");
        QTest::addColumn<int>("cap");
        QTest::addColumn<QList<int>>("expected");
        QTest::newRow("StreamPlayer and LiveStream 1s..30s")
            << 1000 << 30000 << QList<int>{1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
        QTest::newRow("FrigateLive and BaichuanEventListener 5s..60s")
            << 5000 << 60000 << QList<int>{5000, 10000, 20000, 40000, 60000, 60000, 60000};
        QTest::newRow("DeviceManager 15s..300s (seconds)")
            << 15 << 300 << QList<int>{15, 30, 60, 120, 240, 300, 300, 300};
    }
    void sequences()
    {
        QFETCH(int, start);
        QFETCH(int, cap);
        QFETCH(QList<int>, expected);
        Backoff b({start, cap});
        const auto got = failures(b, int(expected.size()));
        QCOMPARE(QList<int>(got.begin(), got.end()), expected);
    }

    void resetRestartsTheSequence()
    {
        Backoff b({1000, 30000});
        failures(b, 4);
        b.reset();
        QCOMPARE(b.fail(), 1000);
        QCOMPARE(b.fail(), 2000);
    }

    void currentDoesNotAdvance()
    {
        Backoff b({5000, 60000});
        QCOMPARE(b.current(), 5000);
        QCOMPARE(b.current(), 5000);
        b.fail();
        QCOMPARE(b.current(), 10000);
    }

    // BaichuanEventListener: a refused connection waits a fixed 600 s, and later plain
    // failures keep waiting 600 s until a connection succeeds (which resets to 5 s).
    void overrideOutsideTheCapIsSticky()
    {
        Backoff b({5000, 60000});
        QCOMPARE(b.fail(), 5000);
        b.setDelay(600000);
        QCOMPARE(b.fail(), 600000);
        QCOMPARE(b.fail(), 600000);
        b.reset();
        QCOMPARE(b.fail(), 5000);
    }

    // Injected time: the caller supplies "now", so no real clock is involved.
    void failAtUsesTheInjectedNow()
    {
        Backoff b({15, 300});
        QCOMPARE(b.failAt(1000), qint64(1015));
        QCOMPARE(b.failAt(1015), qint64(1045));
        QCOMPARE(b.failAt(5000), qint64(5060));
    }

    // DeviceManager keeps its per-host delay in a hash, so it uses the stateless step.
    void statelessStepMatchesTheStatefulSequence()
    {
        const BackoffPolicy p{15, 300};
        Backoff b(p);
        int d = p.start;
        for (int i = 0; i < 10; ++i) {
            QCOMPARE(d, b.fail());
            d = p.grow(d);
        }
    }

    void growthFactor()
    {
        Backoff b({100, 1000, 3});
        QCOMPARE(failures(b, 4) == std::vector<int>({100, 300, 900, 1000}), true);
    }

    // LiveStream's Baichuan-main retry is a fixed 1500 ms: growth 1 never grows.
    void fixedDelay()
    {
        Backoff b({1500, 1500, 1});
        QCOMPARE(failures(b, 3) == std::vector<int>({1500, 1500, 1500}), true);
    }
};

QTEST_APPLESS_MAIN(BackoffTest)
#include "test_backoff.moc"
