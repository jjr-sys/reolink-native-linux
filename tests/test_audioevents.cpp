#include "device/AudioEventApi.h"

#include <QtTest>

using namespace rl::audio;
using rl::Json;

static Json J(QByteArray s) { return Json::parse(s.replace('\'', '"').toStdString()); }

static Event ev(const char *id, double t, const char *cam = "kitchen", bool flagged = true)
{
    Event e;
    e.triggers = QStringList{flagged ? "raised_voice" : "speech"};
    e.id = id;
    e.time = t;
    e.camera = cam;
    return e;
}

class TestAudioEvents : public QObject
{
    Q_OBJECT
private slots:
    void parse_raisedVoiceEvent()
    {
        const auto v = parseEvents(J("[{'id':'1790000000-ab12cd34','time':1790000000.5,'camera':'kitchen',"
                                     "'triggers':['raised_voice'],'scores':{'Shout':0.61,'Yell':0.42},"
                                     "'transcript':null,'has_clip':true}]"));
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0].id, QString("1790000000-ab12cd34"));
        QCOMPARE(v[0].camera, QString("kitchen"));
        QCOMPARE(v[0].triggers, QStringList{"raised_voice"});
        QCOMPARE(v[0].scoreText, QString("Shout 61%"));
        QVERIFY(v[0].transcript.isEmpty());
        QVERIFY(v[0].hasClip);
    }
    void parse_threatSpeechEvent()
    {
        const auto v = parseEvents(J("[{'id':'a-b','time':5,'camera':'kitchen','triggers':['threat_speech'],"
                                     "'scores':{'phrases':2,'categories':['threat','abuse']},"
                                     "'transcript':'i will hurt you','has_clip':false}]"));
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0].transcript, QString("i will hurt you"));
        QCOMPARE(v[0].scoreText, QString("2 phrase(s) · threat, abuse"));
        QVERIFY(!v[0].hasClip);
    }
    void parse_dropsMalformedAndTolerantOfBadShapes()
    {
        QVERIFY(parseEvents(Json()).isEmpty());
        QVERIFY(parseEvents(J("{}")).isEmpty());
        const auto v = parseEvents(J("[{'time':1,'camera':'k'},{'id':'x','camera':'k'},{'id':'y','time':2},"
                                     "{'id':'ok','time':3,'camera':'k','triggers':'oops','scores':7}]"));
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0].id, QString("ok"));
        QVERIFY(v[0].triggers.isEmpty());
        QVERIFY(v[0].scoreText.isEmpty());
    }
    void status_readsPausedAndReason()
    {
        auto s = parseStatus(J("{'transcription_paused':true,'paused_reason':'sustained_load load1=4.2'}"));
        QVERIFY(s.ok && s.transcriptionPaused);
        QCOMPARE(s.pausedReason, QString("sustained_load load1=4.2"));
        s = parseStatus(J("{'transcription_paused':false,'paused_reason':null}"));
        QVERIFY(s.ok && !s.transcriptionPaused && s.pausedReason.isEmpty());
        QVERIFY(!parseStatus(J("{'error':'unauthorized'}")).ok);
    }
    void triggerText_plainSpeech()
    {
        QCOMPARE(triggerText({"speech"}), QString("Speech"));
        QVERIFY(!ev("a", 1, "k", false).flagged());
        QVERIFY(ev("a", 1).flagged());
    }
    void url_paging()
    {
        QCOMPARE(eventsUrl("h", 8090), QString("http://h:8090/events?limit=100"));
        QCOMPARE(eventsUrl("h", 8090, 200, 1790000000.5, true),
                 QString("http://h:8090/events?limit=200&before=1790000000.500&flagged=1"));
    }
    void tracker_plainSpeechNeverAlertsOrNotifies()
    {
        AlertTracker t;
        t.update({}, 1000);
        const auto fresh = t.update({ev("s", 0, "patio", false)}, 1000);
        QVERIFY(fresh.isEmpty());
        QVERIFY(!t.active("patio", 1000));
    }
    void triggerText_names()
    {
        QCOMPARE(triggerText({"raised_voice"}), QString("Loud voice"));
        QCOMPARE(triggerText({"threat_speech"}), QString("Threatening words"));
        QCOMPARE(triggerText({"raised_voice", "threat_speech"}), QString("Loud voice + Threatening words"));
        QCOMPARE(triggerText({}), QString("Audio event"));
    }
    void urls_encodeEventId()
    {
        QCOMPARE(clipUrl("h", 8090, "1-ab"), QString("http://h:8090/events/1-ab/clip"));
        QCOMPARE(clipUrl("h", 8090, "../x"), QString("http://h:8090/events/..%2Fx/clip"));
    }

    // ---- alert window ----
    void tracker_backlogLightsBorderButDoesNotNotify()
    {
        AlertTracker t;
        const double now = 10000;
        const auto fresh = t.update({ev("new", now - 60), ev("old", now - 4000)}, now);
        QVERIFY(fresh.isEmpty());
        QVERIFY(t.active("kitchen", now));           // 60 s ago: still inside 5 min
        QVERIFY(!t.active("kitchen", now + 241));    // clears 300 s after that event
    }
    void tracker_oldBacklogDoesNotLightBorder()
    {
        AlertTracker t;
        t.update({ev("old", 10000 - 4000)}, 10000);
        QVERIFY(!t.active("kitchen", 10000));
    }
    void tracker_laterPollReturnsNewEventsOldestFirst()
    {
        AlertTracker t;
        t.update({ev("a", 100)}, 1000);
        const auto fresh = t.update({ev("c", 1500), ev("b", 1490), ev("a", 100)}, 1010);
        QCOMPARE(fresh.size(), 2);
        QCOMPARE(fresh[0].id, QString("b"));
        QCOMPARE(fresh[1].id, QString("c"));
        QVERIFY(t.update({ev("c", 1500), ev("b", 1490), ev("a", 100)}, 1020).isEmpty());
    }
    void tracker_liveEventCountsFromArrivalNotServerClock()
    {
        AlertTracker t;
        t.update({}, 1000);
        t.update({ev("skewed", 1)}, 2000); // server clock far behind
        QVERIFY(t.active("kitchen", 2000 + 299));
        QVERIFY(!t.active("kitchen", 2000 + 301));
    }
    void tracker_furtherDetectionExtendsWindow()
    {
        AlertTracker t;
        t.update({}, 1000);
        t.update({ev("a", 0)}, 1000);
        t.update({ev("b", 0), ev("a", 0)}, 1200); // 200 s in: another one
        QVERIFY(t.active("kitchen", 1200 + 299));
        QVERIFY(!t.active("kitchen", 1200 + 301));
    }
    void tracker_camerasAreIndependentAndExpiryReported()
    {
        AlertTracker t;
        t.update({}, 0);
        t.update({ev("a", 0, "kitchen")}, 100);
        t.update({ev("b", 0, "garage"), ev("a", 0, "kitchen")}, 200);
        QCOMPARE(t.activeCameras(250).size(), 2);
        QCOMPARE(t.nextExpiry(250), 400.0);
        QCOMPARE(t.activeCameras(450), QStringList{"garage"});
        QCOMPARE(t.nextExpiry(600), 0.0);
    }
    void tracker_forgetsIdsThatLeftTheServersList()
    {
        AlertTracker t;
        t.update({}, 0);
        t.update({ev("a", 0)}, 10);
        t.update({}, 20);
        QCOMPARE(t.update({ev("a", 0)}, 30).size(), 1); // gone then back: treated as new
    }
};

QTEST_MAIN(TestAudioEvents)
#include "test_audioevents.moc"
