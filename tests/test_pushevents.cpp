#include "protocol/BcAlarmEvents.h"
#include "protocol/ReolinkApi.h"

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
