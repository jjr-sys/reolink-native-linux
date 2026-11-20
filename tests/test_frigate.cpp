#include "device/FrigateApi.h"
#include "media/LiveUrl.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

using namespace rl::frigate;
using rl::Json;

// JSON written with single quotes (keeps moc happy and the strings readable).
static Json J(QByteArray s) { return Json::parse(s.replace('\'', '"').toStdString()); }

class TestFrigate : public QObject
{
    Q_OBJECT

    // A Frigate stand-in (as test_recorderdays): answers each GET whose path starts with one
    // of `answers`' keys with that status and body; anything else is taken and never answered.
    struct FakeFrigate : QTcpServer
    {
        QHash<QString, QPair<int, QByteArray>> answers;
        FakeFrigate()
        {
            listen(QHostAddress::LocalHost);
            connect(this, &QTcpServer::newConnection, this, [this] {
                QTcpSocket *s = nextPendingConnection();
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    const QString path = QString::fromLatin1(s->readLine().split(' ').value(1));
                    for (auto it = answers.cbegin(); it != answers.cend(); ++it)
                        if (path.startsWith(it.key())) {
                            const QByteArray &body = it.value().second;
                            s->write("HTTP/1.1 " + QByteArray::number(it.value().first)
                                     + " X\r\nContent-Type: application/json\r\nContent-Length: "
                                     + QByteArray::number(body.size())
                                     + "\r\nConnection: close\r\n\r\n" + body);
                            s->disconnectFromHost();
                            return;
                        }
                });
            });
        }
    };

    // The fetches block, so they run on a worker while this thread serves the fake.
    template <typename F>
    static auto offThread(F f)
    {
        decltype(f()) result;
        QThread *worker = QThread::create([&] { result = f(); });
        worker->start();
        while (!worker->wait(5))
            QCoreApplication::processEvents();
        delete worker;
        return result;
    }

private slots:
    // A refused login is Auth, with the wording the Devices list shows.
    void fetch_refusedIsAuth()
    {
        FakeFrigate frigate;
        frigate.answers["/api/config"] = {401, "{}"};
        const quint16 port = frigate.serverPort();
        const auto r = offThread([port] { return fetchConfig("127.0.0.1", port, 5000); });
        QVERIFY(!r.ok());
        QCOMPARE(r.kind, ErrorKind::Auth);
        QCOMPARE(r.error, QStringLiteral("Frigate sign-in failed"));
    }
    // An events reply that isn't a list is a bad reply, not an empty one.
    void fetch_eventsNotAListIsBadReply()
    {
        FakeFrigate frigate;
        frigate.answers["/api/events?after=0&limit=5"] = {200, R"({"detail":"nope"})"};
        const quint16 port = frigate.serverPort();
        const auto r = offThread([port] { return fetchEvents("127.0.0.1", port, 0, 5, 5000); });
        QCOMPARE(r.kind, ErrorKind::BadReply);
        QVERIFY(r.error.isEmpty());
        QVERIFY(r.value.isEmpty());
    }
    void fetch_configNotJsonIsBadReply()
    {
        FakeFrigate frigate;
        frigate.answers["/api/config"] = {200, "<html>router</html>"};
        const quint16 port = frigate.serverPort();
        const auto r = offThread([port] { return fetchConfig("127.0.0.1", port, 5000); });
        QCOMPARE(r.kind, ErrorKind::BadReply);
    }
    void fetch_nothingListeningIsUnreachable()
    {
        quint16 port = 0;
        {
            QTcpServer gone;
            gone.listen(QHostAddress::LocalHost);
            port = gone.serverPort();
        }
        const auto r = offThread([port] { return fetchEvents("127.0.0.1", port, 0, 1, 5000); });
        QCOMPARE(r.kind, ErrorKind::Unreachable);
        QVERIFY(!r.error.isEmpty());
        // Another HTTP error is Unreachable too (as the Devices list has always said).
        FakeFrigate frigate;
        frigate.answers["/api/config"] = {500, "{}"};
        const quint16 p = frigate.serverPort();
        const auto e = offThread([p] { return fetchConfig("127.0.0.1", p, 5000); });
        QCOMPARE(e.kind, ErrorKind::Unreachable);
        QCOMPARE(e.error, QStringLiteral("Frigate returned HTTP 500"));
    }
    // A config with no enabled camera is its own outcome ("Frigate has no enabled cameras").
    void checkServer_noEnabledCameras()
    {
        FakeFrigate frigate;
        frigate.answers["/api/config"] = {200, R"({"cameras":{"yard":{"enabled":false}}})"};
        const quint16 port = frigate.serverPort();
        const auto r = offThread([port] { return checkServer("127.0.0.1", port, {}, {}, 5000); });
        QCOMPARE(r.kind, ErrorKind::NoCameras);
        QVERIFY(r.error.isEmpty());
    }
    void checkServer_okHasTheConfig()
    {
        FakeFrigate frigate;
        frigate.answers["/api/config"] = {200, R"({"cameras":{"yard":{}}})"};
        const quint16 port = frigate.serverPort();
        const auto r = offThread([port] { return checkServer("127.0.0.1", port, {}, {}, 5000); });
        QVERIFY(r.ok());
        QCOMPARE(parseCameraNames(r.value), QStringList{"yard"});
    }
    void fetch_eventsParsed()
    {
        FakeFrigate frigate;
        frigate.answers["/api/events?after=100&limit=50"] =
            {200, R"([{"id":"a","camera":"yard","label":"person","start_time":120.5}])"};
        const quint16 port = frigate.serverPort();
        const auto r = offThread([port] { return fetchEvents("127.0.0.1", port, 100, 50, 5000); });
        QVERIFY(r.ok());
        QCOMPARE(r.value.size(), 1);
        QCOMPARE(r.value.first().camera, QStringLiteral("yard"));
    }
    // /ws: {"topic":"events","payload":"<JSON string>"}; only "new" events count.
    void parsesANewLiveEvent()
    {
        const QByteArray payload =
            R"({"type":"new","before":{},"after":{"id":"1792379281.5-ab","camera":"bosch_camera","label":"person","start_time":1792379281.5,"false_positive":false}})";
        const QByteArray msg = QJsonDocument(QJsonObject{{"topic", "events"},
                                                         {"payload", QString::fromUtf8(payload)}})
                                   .toJson(QJsonDocument::Compact);
        Event e;
        QVERIFY(parseLiveEvent(msg, &e));
        QCOMPARE(e.camera, QStringLiteral("bosch_camera"));
        QCOMPARE(e.label, QStringLiteral("person"));
        QCOMPARE(e.start, 1792379281.5);
        QCOMPARE(e.id, QStringLiteral("1792379281.5-ab"));
    }

    void ignoresOtherLiveMessages()
    {
        Event e;
        auto wrap = [](const char *topic, const QByteArray &payload) {
            return QJsonDocument(QJsonObject{{"topic", topic}, {"payload", QString::fromUtf8(payload)}})
                .toJson(QJsonDocument::Compact);
        };
        QVERIFY(!parseLiveEvent(wrap("events", R"({"type":"update","after":{"camera":"a","label":"person","start_time":5}})"), &e));
        QVERIFY(!parseLiveEvent(wrap("events", R"({"type":"end","after":{"camera":"a","label":"person","start_time":5}})"), &e));
        QVERIFY(!parseLiveEvent(wrap("reviews", R"({"type":"new"})"), &e));
        QVERIFY(!parseLiveEvent(wrap("events", R"({"type":"new","after":{"label":"person","start_time":5}})"), &e));
        QVERIFY(!parseLiveEvent("not json", &e));
    }

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
    // /api/config lists each camera's objects.track; the Events page offers those types
    // (plus Status). Frigate raises no Motion or Visitor events.
    void cameraEventTypes_fromObjectsTrack()
    {
        const Json cfg = J("{ 'cameras': {"
                           " 'yard': {'objects': {'track': ['person', 'car', 'truck', 'dog', 'bird']}},"
                           " 'gate': {'objects': {'track': ['person']}},"
                           " 'bare': {} } }");
        QCOMPARE(cameraEventTypes(cfg, "yard"), QStringList({"person", "vehicle", "pet", "status"}));
        QCOMPARE(cameraEventTypes(cfg, "gate"), QStringList({"person", "status"}));
        // No track list: Frigate's default is person.
        QCOMPARE(cameraEventTypes(cfg, "bare"), QStringList({"person", "status"}));
        // A camera not in the config raises nothing.
        QVERIFY(cameraEventTypes(cfg, "missing").isEmpty());
    }
    void mainRestream_onlyWhenGo2rtcPublishesIt()
    {
        const Json cfg = J("{ 'cameras': {'kitchen': {}, 'bosch_camera': {}}, 'go2rtc': {'streams': {'kitchen': ['rtsp://sub'], 'kitchen_main': ['rtsp://main'], 'bosch_camera': ['rtsp://y']}} }");
        QCOMPARE(mainRestream(cfg, "kitchen"), QString("kitchen_main"));
        QVERIFY(mainRestream(cfg, "bosch_camera").isEmpty());
        QVERIFY(mainRestream(J("{'cameras': {'kitchen': {}}}"), "kitchen").isEmpty());
        QVERIFY(mainRestream(J("{'go2rtc': {'streams': null}}"), "kitchen").isEmpty());
        QVERIFY(mainRestream(cfg, "").isEmpty());
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
    void liveUrl_restreamLogin()
    {
        // go2rtc's RTSP restream can require its own login (Basic), which is not the Frigate login.
        QCOMPARE(liveUrl("h", 8971, "bosch_camera", "outpost", "p@ss:w/rd"),
                 QString("rtsp://outpost:p%40ss%3Aw%2Frd@h:8554/bosch_camera"));
        QCOMPARE(liveUrl("h", 8971, "a b", "u s", "x"),
                 QString("rtsp://u%20s:x@h:8554/a%20b"));
        // No user means no login at all, exactly as before.
        QCOMPARE(liveUrl("h", 8971, "cam", "", "ignored"), QString("rtsp://h:8554/cam"));
        QCOMPARE(liveUrl("h", 8971, "cam", "", ""), liveUrl("h", 8971, "cam"));
        // A user with an empty password still sends the user.
        QCOMPARE(liveUrl("h", 8971, "cam", "u", ""), QString("rtsp://u:@h:8554/cam"));
    }
    void urls()
    {
        QCOMPARE(liveUrl("192.168.7.204", 5000, "bosch_camera"),
                 QString("rtsp://192.168.7.204:8554/bosch_camera"));
        QCOMPARE(liveUrl("h", 5000, "a b&c"),
                 QString("rtsp://h:8554/a%20b%26c"));
        QCOMPARE(eventsUrl("h", 5000, 12.5, 50),
                 QString("http://h:5000/api/events?after=12.5&limit=50"));
        QCOMPARE(latestFrameUrl("h", 5000, "cam"), QString("http://h:5000/api/cam/latest.jpg"));
        QCOMPARE(configUrl("h", 5000), QString("http://h:5000/api/config"));
        QCOMPARE(clipUrl("h", 5000, "cam", 1000),
                 QString("http://h:5000/api/cam/start/975/end/1020/clip.mp4"));
        // The Events clip player asks for its own window (step 22).
        QCOMPARE(clipUrl("h", 5000, "cam", 1000, 15, 30),
                 QStringLiteral("http://h:5000/api/cam/start/985/end/1030/clip.mp4"));
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
    // Playback asks for one hour from the playhead (wayfinder ticket 23): Frigate's VOD refuses a
    // list of more than about 1,000 segments ("invalid number of elements in the durations array
    // 1081" for 3 h of 10 s segments), and Playback reopens at the next second when a list ends.
    // A camera added to or removed from Frigate shows up in the app without a restart (wayfinder
    // ticket 23): the host is rebuilt only when the list really changed.
    void cameraList_changedIgnoresOrder()
    {
        const QStringList shown{"bosch_camera", "kitchen"};
        QVERIFY(!cameraListChanged(shown, {"kitchen", "bosch_camera"}));
        QVERIFY(cameraListChanged(shown, {"bosch_camera"}));
        QVERIFY(cameraListChanged(shown, {"bosch_camera", "kitchen", "garage"}));
        QVERIFY(cameraListChanged({"bosch_camera"}, {"garage"}));
        // An unreadable config (no cameras parsed) is not a change: keep what's shown.
        QVERIFY(!cameraListChanged(shown, {}));
    }
    // The list is re-read every five minutes, and on the first poll of a host never read.
    void cameraList_dueEveryFiveMinutes()
    {
        QVERIFY(cameraListDue(0, 1000));            // never checked
        QVERIFY(!cameraListDue(1000, 1000 + 299));
        QVERIFY(cameraListDue(1000, 1000 + 300));
    }

    // A Frigate camera keeps its number for good, by name (wayfinder ticket 24): removing one
    // never renumbers the others, so their saved settings, picks and events stay with them.
    void channels_firstReadNumbersInListOrder()
    {
        const auto ch = assignChannels({"bosch_camera", "garage", "kitchen"}, {}, {});
        QCOMPARE(ch.value("bosch_camera"), 0);
        QCOMPARE(ch.value("garage"), 1);
        QCOMPARE(ch.value("kitchen"), 2);
        // An install from before names were kept: today's numbering stands, whatever rows exist.
        QCOMPARE(assignChannels({"bosch_camera"}, {}, {0, 1}).value("bosch_camera"), 0);
    }
    void channels_removingOneKeepsTheOthers()
    {
        const QHash<QString, int> known{{"bosch_camera", 0}, {"garage", 1}, {"kitchen", 2}};
        const auto ch = assignChannels({"bosch_camera", "kitchen"}, known, {0, 1, 2});
        QCOMPARE(ch.size(), 2);
        QCOMPARE(ch.value("bosch_camera"), 0);
        QCOMPARE(ch.value("kitchen"), 2);
    }
    // A new camera takes the lowest number no camera has used on this server (a number with
    // old history or settings is never handed to a different camera); one that comes back
    // gets its own number again.
    void channels_newCameraTakesTheLowestUnusedNumber()
    {
        const QHash<QString, int> known{{"bosch_camera", 0}, {"kitchen", 2}};
        auto ch = assignChannels({"bosch_camera", "garage", "kitchen"}, known, {0, 2});
        QCOMPARE(ch.value("garage"), 1);
        ch = assignChannels({"bosch_camera", "garage"}, {{"bosch_camera", 0}}, {0, 1});
        QCOMPARE(ch.value("garage"), 2);   // 1 has an unnamed row (old history): skipped
        ch = assignChannels({"bosch_camera", "kitchen"}, {{"bosch_camera", 0}, {"kitchen", 1}}, {0, 1});
        QCOMPARE(ch.value("kitchen"), 1);
    }

    void playbackVod_isOneHourFromThePlayhead()
    {
        QCOMPARE(playbackVodUrl("h", 5000, "cam", 1794510357),
                 QString("http://h:5000/vod/cam/start/1794510357/end/1794513957/index.m3u8"));
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
