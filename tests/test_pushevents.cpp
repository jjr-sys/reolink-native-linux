#include "protocol/BcAlarmEvents.h"
#include "protocol/ReolinkApi.h"
#include "protocol/WebSocketFrames.h"

#include <QtTest>

using namespace rl;

namespace {
// The shape an RLN36 pushes (cmd 33): every channel, each with status and AItype.
const QByteArray kBody = R"(<?xml version="1.0" encoding="UTF-8" ?>
<body>
<AlarmEventList version="1.1">
<AlarmEvent version="1.1">
<channelId>0</channelId>
<status>none</status>
<recording>0</recording>
<AItype>none</AItype>
<timeStamp>0</timeStamp>
</AlarmEvent>
<AlarmEvent version="1.1">
<channelId>2</channelId>
<status>MD</status>
<recording>1</recording>
<AItype>people,vehicle</AItype>
<timeStamp>0</timeStamp>
</AlarmEvent>
<AlarmEvent version="1.1">
<channelId>3</channelId>
<status>MD,visitor</status>
<AItype>people</AItype>
</AlarmEvent>
<AlarmEvent version="1.1">
<channelId>4</channelId>
<status>MD</status>
<AItype>other</AItype>
<smartAiTypeList><smartAiType>crossline</smartAiType></smartAiTypeList>
</AlarmEvent>
<AlarmEvent version="1.1">
<channelId>5</channelId>
<status>none</status>
<AItype>dog_cat</AItype>
</AlarmEvent>
<AlarmEvent version="1.1">
<status>MD</status>
</AlarmEvent>
</AlarmEventList>
<DayNightEvent version="1.1"><channelId>0</channelId><mode>day</mode></DayNightEvent>
</body>
)";

api::DetectionState state(bool motion, bool person, bool vehicle, bool pet, bool visitor = false)
{
    api::DetectionState s;
    s.motion = motion;
    s.person = person;
    s.vehicle = vehicle;
    s.pet = pet;
    s.visitor = visitor;
    return s;
}
} // namespace

class TestPushEvents : public QObject
{
    Q_OBJECT
private slots:
    void parsesEveryChannelAndSkipsOneWithoutAChannel()
    {
        const auto a = bc::parseAlarmEvents(kBody);
        QCOMPARE(a.size(), 5);
        QCOMPARE(a[0].channel, 0);
        QVERIFY(!a[0].state.motion && !a[0].state.person && !a[0].state.visitor);
    }

    void readsMotionAndSeveralAiTypes()
    {
        const auto a = bc::parseAlarmEvents(kBody);
        QCOMPARE(a[1].channel, 2);
        QVERIFY(a[1].state.motion);
        QVERIFY(a[1].state.person);
        QVERIFY(a[1].state.vehicle);
        QVERIFY(!a[1].state.pet);
    }

    void readsADoorbellPress()
    {
        const auto a = bc::parseAlarmEvents(kBody);
        QCOMPARE(a[2].channel, 3);
        QVERIFY(a[2].state.visitor);
        QVERIFY(a[2].state.motion);
        QVERIFY(a[2].state.person);
    }

    void ignoresTypesWeDoNotShowAndReadsPets()
    {
        const auto a = bc::parseAlarmEvents(kBody);
        QVERIFY(a[3].state.motion);
        QVERIFY(!a[3].state.person && !a[3].state.vehicle && !a[3].state.pet);
        QVERIFY(a[4].state.pet);
        QVERIFY(!a[4].state.motion);
    }

    void emptyOrForeignBodiesGiveNothing()
    {
        QVERIFY(bc::parseAlarmEvents(QByteArray()).isEmpty());
        QVERIFY(bc::parseAlarmEvents("<body><DayNightEvent/></body>").isEmpty());
    }

    // The edge rule both the poll and push go through.
    void risingEdgesOnly()
    {
        const auto none = state(false, false, false, false);
        QCOMPARE(api::risingDetections(none, state(false, true, true, false)),
                 (QStringList{"person", "vehicle"}));
        QVERIFY(api::risingDetections(state(false, true, false, false),
                                      state(false, true, false, false)).isEmpty());
        QCOMPARE(api::risingDetections(state(false, true, false, false),
                                       state(false, true, false, true)),
                 QStringList{"pet"});
    }

    void motionCountsOnlyWithoutAi()
    {
        const auto none = state(false, false, false, false);
        QCOMPARE(api::risingDetections(none, state(true, false, false, false)), QStringList{"motion"});
        QCOMPARE(api::risingDetections(none, state(true, true, false, false)), QStringList{"person"});
        // Motion that stays after the person has gone is the same activity, not new motion.
        QVERIFY(api::risingDetections(state(true, true, false, false),
                                      state(true, false, false, false)).isEmpty());
    }

    // RFC 6455 §1.3's worked example.
    void websocketAcceptKey()
    {
        QCOMPARE(ws::acceptFor("dGhlIHNhbXBsZSBub25jZQ=="), QByteArray("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    }

    void websocketClientFramesRoundTrip()
    {
        for (int n : {0, 5, 125, 126, 300, 70000}) {
            const QByteArray payload(n, 'x');
            QByteArray buf = ws::clientFrame(ws::Text, payload, 0x37fa213d) + "tail";
            QCOMPARE(quint8(buf[1]) & 0x80, 0x80); // clients mask
            ws::Frame f;
            QVERIFY(ws::takeFrame(buf, f));
            QVERIFY(f.fin);
            QCOMPARE(f.opcode, quint8(ws::Text));
            QCOMPARE(f.payload, payload);
            QCOMPARE(buf, QByteArray("tail"));
        }
    }

    void websocketServerFramesAndPartialReads()
    {
        // RFC 6455 §5.7: an unmasked "Hello", then a ping, arriving in pieces.
        const QByteArray wire = QByteArray::fromHex("810548656c6c6f") + QByteArray::fromHex("8900");
        QByteArray buf;
        ws::Frame f;
        for (int i = 0; i < 6; ++i) {
            buf += wire[i];
            QVERIFY(!ws::takeFrame(buf, f));
        }
        buf += wire.mid(6);
        QVERIFY(ws::takeFrame(buf, f));
        QCOMPARE(f.payload, QByteArray("Hello"));
        QVERIFY(ws::takeFrame(buf, f));
        QCOMPARE(f.opcode, quint8(ws::Ping));
        QVERIFY(buf.isEmpty());
        // A first fragment, not final.
        QByteArray frag = QByteArray::fromHex("01034865 6c");
        QVERIFY(ws::takeFrame(frag, f));
        QVERIFY(!f.fin);
        QCOMPARE(f.payload, QByteArray("Hel"));
    }

    void aDoorbellPressRisesOnce()
    {
        const auto press = state(true, true, false, false, true);
        QCOMPARE(api::risingDetections(state(false, false, false, false), press),
                 (QStringList{"visitor", "person"}));
        QVERIFY(api::risingDetections(press, press).isEmpty());
        QCOMPARE(api::risingDetections(state(true, true, false, false), press), QStringList{"visitor"});
    }
};

QTEST_GUILESS_MAIN(TestPushEvents)
#include "test_pushevents.moc"
