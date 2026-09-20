#include "device/FrigateApi.h"
#include "media/LiveUrl.h"

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
        QCOMPARE(clipUrl("h", 5000, "cam", 1000),
                 QString("http://h:5000/api/cam/start/975/end/1020/clip.mp4"));
    }
    void bursts_foldEventsWithinThirtySeconds()
    {
        QVERIFY(startsNewBurst(0, 1000));         // nothing raised yet
        QVERIFY(!startsNewBurst(1000, 1010));
        QVERIFY(!startsNewBurst(1000, 1029.9));
        QVERIFY(startsNewBurst(1000, 1030));
    }
    void recordings_parseAndMergeIntoContinuousRanges()
    {
        const auto segs = parseRecordings(J("[{'start_time':100,'end_time':110},{'start_time':110.2,'end_time':120},{'start_time':300,'end_time':310},{'id':'x'},{'start_time':5,'end_time':5}]"));
        QCOMPARE(segs.size(), 3); // the malformed and the zero-length entries are dropped
        const auto merged = mergeRecordings(segs);
        QCOMPARE(merged.size(), 2);
        QCOMPARE(merged[0].start, 100.0);
        QCOMPARE(merged[0].end, 120.0);
        QCOMPARE(merged[1].start, 300.0);
    }
    void recordings_mergeSortsItsInput()
    {
        QVector<Recording> v;
        Recording a; a.start = 20; a.end = 30; v.append(a);
        Recording b; b.start = 10; b.end = 20; v.append(b);
        const auto m = mergeRecordings(v);
        QCOMPARE(m.size(), 1);
        QCOMPARE(m[0].start, 10.0);
        QCOMPARE(m[0].end, 30.0);
    }
    void recordingDays_onlyDaysOfThatMonthWithFootage()
    {
        const Json sum = J("[{'day':'2026-09-20','hours':[{'hour':'17','duration':3000}]},{'day':'2026-09-19','hours':[{'hour':'23','duration':0}]},{'day':'2026-08-31','hours':[{'hour':'01','duration':50}]},{'day':'2026-09-05','hours':[{'hour':'09','duration':10},{'hour':'10','duration':10}]}]");
        const QList<int> d = parseRecordingDays(sum, 2026, 9);
        QCOMPARE(d, (QList<int>{5, 20})); // 19th has an hour entry but no recorded time
        QVERIFY(parseRecordingDays(Json(), 2026, 9).isEmpty());
    }
    void recordingUrls()
    {
        QCOMPARE(recordingsUrl("h", 5000, "cam", 10, 20), QString("http://h:5000/api/cam/recordings?after=10&before=20"));
        QCOMPARE(summaryUrl("h", 5000, "cam", "Australia/Perth"), QString("http://h:5000/api/cam/recordings/summary?timezone=Australia%2FPerth"));
        QCOMPARE(vodUrl("h", 5000, "cam", 100, 200), QString("http://h:5000/vod/cam/start/100/end/200/index.m3u8"));
    }
    // A Frigate live stream must reconnect when it drops; a clip must play once and stop.
    void liveUrls_frigateLiveReconnectsButClipsDoNot()
    {
        QVERIFY(rl::isLiveStreamUrl(liveUrl("h", 5000, "cam")));
        QVERIFY(rl::isLiveStreamUrl("rtsp://h/x"));
        QVERIFY(!rl::isLiveStreamUrl(clipUrl("h", 5000, "cam", 1000)));
        QVERIFY(!rl::isLiveStreamUrl("http://h/some/file.mp4"));
        QVERIFY(!rl::isLiveStreamUrl("/tmp/file.mp4"));
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
