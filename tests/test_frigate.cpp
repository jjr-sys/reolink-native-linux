#include "device/FrigateApi.h"

#include <QtTest>

using namespace rl::frigate;
using rl::Json;

// JSON written with single quotes (keeps moc happy and the strings readable).
static Json J(QByteArray s) { return Json::parse(s.replace('\'', '"').toStdString()); }

class TestFrigate : public QObject
{
    Q_OBJECT
private slots:
    void cameras_onlyEnabledOnesInConfigOrder()
    {
        const Json cfg = J("{ 'cameras': {'bosch_camera': {'enabled': true}, 'garage': {}, 'old': {'enabled': false}}, 'go2rtc': {'streams': {'mulga-nvr_0': ['rtsp://x'], 'bosch_camera': ['rtsp://y']}} }");
        const QStringList names = parseCameraNames(cfg);
        QCOMPARE(names.size(), 2);
        QVERIFY(names.contains("bosch_camera"));
        QVERIFY(names.contains("garage"));
        QVERIFY(!names.contains("old"));
        QVERIFY(!names.contains("mulga-nvr_0")); // go2rtc-only streams are not cameras
    }
    void cameras_missingOrBadConfigIsEmpty()
    {
        QVERIFY(parseCameraNames(Json()).isEmpty());
        QVERIFY(parseCameraNames(J("{'cameras': null}")).isEmpty());
    }
    void events_readFields()
    {
        const Json v = J("[ {'id':'1789867516.7-abc','camera':'bosch_camera','label':'person','start_time':1789867516.757,'end_time':null}, {'id':'2','camera':'bosch_camera','label':'car','start_time':1789867600.5,'end_time':1789867610.0}]");
        const auto ev = parseEvents(v);
        QCOMPARE(ev.size(), 2);
        QCOMPARE(ev[0].id, QString("1789867516.7-abc"));
        QCOMPARE(ev[0].camera, QString("bosch_camera"));
        QCOMPARE(ev[0].label, QString("person"));
        QVERIFY(qAbs(ev[0].start - 1789867516.757) < 1e-6);
        QCOMPARE(ev[1].label, QString("car"));
    }
    void events_dropMalformedEntries()
    {
        QVERIFY(parseEvents(J("{'detail':'nope'}")).isEmpty());
        QCOMPARE(parseEvents(J("[{'camera':'a'},{'id':'x','camera':'a','label':'dog','start_time':5}]")).size(), 1);
    }
    void labels_mapToTheAppsTypes()
    {
        QCOMPARE(typeForLabel("person"), QString("person"));
        for (const char *l : {"car", "truck", "bus", "motorcycle", "bicycle"})
            QCOMPARE(typeForLabel(l), QString("vehicle"));
        QCOMPARE(typeForLabel("dog"), QString("pet"));
        QCOMPARE(typeForLabel("cat"), QString("pet"));
        QCOMPARE(typeForLabel("package"), QString()); // not a detection the app models
    }
    void urls()
    {
        QCOMPARE(liveUrl("192.168.7.204", 5000, "bosch_camera"),
                 QString("http://192.168.7.204:5000/api/go2rtc/api/stream.flv?src=bosch_camera"));
        QCOMPARE(liveUrl("h", 5000, "a b&c"),
                 QString("http://h:5000/api/go2rtc/api/stream.flv?src=a%20b%26c"));
        QCOMPARE(eventsUrl("h", 5000, 12.5, 50),
                 QString("http://h:5000/api/events?after=12.5&limit=50"));
        QCOMPARE(latestFrameUrl("h", 5000, "cam"), QString("http://h:5000/api/cam/latest.jpg"));
        QCOMPARE(configUrl("h", 5000), QString("http://h:5000/api/config"));
    }
    void bursts_foldEventsWithinThirtySeconds()
    {
        QVERIFY(startsNewBurst(0, 1000));         // nothing raised yet
        QVERIFY(!startsNewBurst(1000, 1010));
        QVERIFY(!startsNewBurst(1000, 1029.9));
        QVERIFY(startsNewBurst(1000, 1030));
    }
    // The first poll must not replay history: the watermark is the newest event's start.
    void watermark_advancesOnlyPastEventsSeen()
    {
        QCOMPARE(newWatermark(0, {}), 0.0);
        QVector<Event> ev;
        Event a; a.start = 10; ev.append(a);
        Event b; b.start = 30; ev.append(b);
        QCOMPARE(newWatermark(5, ev), 30.0);
        QCOMPARE(newWatermark(50, ev), 50.0);
    }
};

QTEST_GUILESS_MAIN(TestFrigate)
#include "test_frigate.moc"
