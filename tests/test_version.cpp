#include "core/VersionCompare.h"

#include <QtTest>

// The fork versions itself "X.Y.Z-jjr" and tags releases "vX.Y.Z"; the update check
// must treat those as the same version and only offer a strictly later one.
class TestVersion : public QObject
{
    Q_OBJECT
private slots:
    void forkBuildIsNotOfferedItsOwnTag()
    {
        QVERIFY(!rl::isNewer("v0.3.0", "0.3.0-jjr"));
        QVERIFY(!rl::isNewer("v0.3.0-jjr", "0.3.0-jjr"));
        QVERIFY(!rl::isNewer("0.3.0", "0.3.0"));
    }
    void laterTagIsOffered()
    {
        QVERIFY(rl::isNewer("v0.3.1", "0.3.0-jjr"));
        QVERIFY(rl::isNewer("v0.4.0", "0.3.0-jjr"));
        QVERIFY(rl::isNewer("v1.0.0", "0.3.9-jjr"));
        QVERIFY(rl::isNewer("v0.10.0", "0.9.0-jjr")); // numeric, not string, comparison
    }
    void olderTagIsNotOffered()
    {
        QVERIFY(!rl::isNewer("v0.2.0", "0.3.0-jjr"));
        QVERIFY(!rl::isNewer("v0.1.8", "0.3.0-jjr")); // upstream's last release
    }
};

QTEST_GUILESS_MAIN(TestVersion)
#include "test_version.moc"
