#include "device/DeviceKind.h"
#include "protocol/ReolinkApi.h"

#include <QtTest>

using namespace rl;
using namespace rl::device;

// What a host kind can do and how its cameras are addressed. The capability table is
// the contract DeviceManager, EventManager and the QML pages read instead of comparing
// kind strings.
class TestDeviceKind : public QObject
{
    Q_OBJECT

private slots:
    // A live pane's cost against its Site budget (wayfinder/site-tabs-redesign).
    void streamCost()
    {
        // Reolink: the stream's configured GetEnc bitRate, in kbps.
        QCOMPARE(streamCostMbps(Kind::Nvr, false, 512), 0.512);
        QCOMPARE(streamCostMbps(Kind::Camera, true, 6144), 6.144);
        // Not known (GetEnc missing, or the host listed over Baichuan): 0.5 sub, 6 HD.
        QCOMPARE(streamCostMbps(Kind::Nvr, false, 0), 0.5);
        QCOMPARE(streamCostMbps(Kind::Nvr, true, -1), 6.0);
        // Frigate: fixed defaults, whatever is passed.
        QCOMPARE(streamCostMbps(Kind::Frigate, false, 2048), 0.5);
        QCOMPARE(streamCostMbps(Kind::Frigate, true, 0), 6.0);
        // A bare stream has one quality.
        QCOMPARE(streamCostMbps(Kind::Stream, false, 0), 1.0);
        QCOMPARE(streamCostMbps(Kind::Stream, true, 9000), 1.0);
    }

    void kindStringsRoundTrip_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("kind");
        QTest::newRow("camera") << "camera" << int(Kind::Camera);
        QTest::newRow("nvr") << "nvr" << int(Kind::Nvr);
        QTest::newRow("stream") << "stream" << int(Kind::Stream);
        QTest::newRow("frigate") << "frigate" << int(Kind::Frigate);
    }
    void kindStringsRoundTrip()
    {
        QFETCH(QString, text);
        QFETCH(int, kind);
        QCOMPARE(int(kindFromString(text)), kind);
        QCOMPARE(kindToString(Kind(kind)), text);
    }
    void unknownKindIsACamera()
    {
        // The Database kind column is free text; a row from a future build or a
        // blank one must still behave as the default (a Reolink camera).
        QCOMPARE(kindFromString(QString()), Kind::Camera);
        QCOMPARE(kindFromString(QStringLiteral("push")), Kind::Camera);
    }

    void capabilityTable_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("reolink");
        QTest::addColumn<bool>("multiChannel");
        QTest::addColumn<bool>("alwaysOnline");
        QTest::addColumn<bool>("frigate");
        QTest::addColumn<int>("detections");
        QTest::addColumn<bool>("baichuanDownload");
        QTest::newRow("camera") << int(Kind::Camera) << true << false << false << false
                                << int(DetectionSource::ReolinkPollAndPush) << false;
        QTest::newRow("nvr") << int(Kind::Nvr) << true << true << false << false
                             << int(DetectionSource::ReolinkPollAndPush) << true;
        QTest::newRow("stream") << int(Kind::Stream) << false << false << true << false
                                << int(DetectionSource::None) << false;
        QTest::newRow("frigate") << int(Kind::Frigate) << false << true << false << true
                                 << int(DetectionSource::FrigateServer) << false;
    }
    void capabilityTable()
    {
        QFETCH(int, kind);
        QFETCH(bool, reolink);
        QFETCH(bool, multiChannel);
        QFETCH(bool, alwaysOnline);
        QFETCH(bool, frigate);
        QFETCH(int, detections);
        QFETCH(bool, baichuanDownload);
        const Capabilities c = capabilities(Kind(kind));
        QCOMPARE(c.reolinkProtocol, reolink);
        QCOMPARE(c.direct(), !reolink);
        QCOMPARE(c.multiChannel, multiChannel);
        QCOMPARE(c.alwaysOnline, alwaysOnline);
        QCOMPARE(c.frigateApi, frigate);
        QCOMPARE(int(c.detections), detections);
        QCOMPARE(c.baichuanDownload, baichuanDownload);
    }
    void capabilityMapForQml()
    {
        const QVariantMap m = capabilityMap(Kind::Frigate);
        QCOMPARE(m.value("multiChannel").toBool(), true);
        QCOMPARE(m.value("direct").toBool(), true);
        QCOMPARE(m.value("frigateApi").toBool(), true);
        QCOMPARE(m.value("reolinkProtocol").toBool(), false);
    }

    // Playback: a bare stream has no recordings of its own; every recorder kind does.
    void onlyBareStreamsHaveNoRecordings()
    {
        QVERIFY(capabilities(Kind::Camera).hasRecordings);
        QVERIFY(capabilities(Kind::Nvr).hasRecordings);
        QVERIFY(capabilities(Kind::Frigate).hasRecordings);
        QVERIFY(!capabilities(Kind::Stream).hasRecordings);
        QCOMPARE(capabilityMap(Kind::Stream).value("hasRecordings").toBool(), false);
        QCOMPARE(capabilityMap(Kind::Nvr).value("hasRecordings").toBool(), true);
    }

    void boschFeedIsRecognisedByName()
    {
        QVERIFY(looksLikeBoschFeed(QStringLiteral("Bosch Camera")));
        QVERIFY(looksLikeBoschFeed(QStringLiteral("front BOSCH")));
        QVERIFY(!looksLikeBoschFeed(QStringLiteral("Driveway")));
        QVERIFY(!looksLikeBoschFeed(QString()));
    }

    void frigateLiveUrlCarriesTheRestreamLogin()
    {
        LiveSource s;
        s.kind = Kind::Frigate;
        s.addr = "h";
        s.port = 8971;
        s.chanName = "bosch_camera";
        QCOMPARE(liveUrl(s, true), QString("rtsp://h:8554/bosch_camera"));
        s.restreamUser = "outpost";
        s.restreamPassword = "pw";
        QCOMPARE(liveUrl(s, true), QString("rtsp://outpost:pw@h:8554/bosch_camera"));
        QCOMPARE(liveUrl(s, false), QString("rtsp://outpost:pw@h:8554/bosch_camera"));
    }
    void frigateHdPlaysTheMainRestreamWhenThereIsOne()
    {
        LiveSource s;
        s.kind = Kind::Frigate;
        s.addr = "h";
        s.chanName = "kitchen";
        s.mainRestream = "kitchen_main";
        QCOMPARE(liveUrl(s, true), QString("rtsp://h:8554/kitchen_main"));
        QCOMPARE(liveUrl(s, false), QString("rtsp://h:8554/kitchen"));
    }
    void liveUrlPerKind()
    {
        LiveSource s;
        s.addr = QStringLiteral("10.0.0.5");
        s.port = 443;
        s.username = QStringLiteral("admin");
        s.password = QStringLiteral("secret");
        s.channel = 2;
        s.mainCodec = QStringLiteral("h265");

        // Reolink: needs credentials loaded (primed); sub stream is always h264.
        s.kind = Kind::Camera;
        QVERIFY(liveUrl(s, true).isEmpty());
        s.primed = true;
        QCOMPARE(liveUrl(s, true), api::rtspUrl(s.addr, s.username, s.password, 2, true, "h265"));
        QCOMPARE(liveUrl(s, false), api::rtspUrl(s.addr, s.username, s.password, 2, false, "h264"));
        s.kind = Kind::Nvr;
        QCOMPARE(liveUrl(s, true), api::rtspUrl(s.addr, s.username, s.password, 2, true, "h265"));

        // Frigate: restream by camera key, never needs priming.
        s.kind = Kind::Frigate;
        s.primed = false;
        s.chanName = QStringLiteral("front");
        QVERIFY(liveUrl(s, true).contains(QStringLiteral("front")));
        QCOMPARE(liveUrl(s, true), liveUrl(s, false));

        // Stream: the saved URL, credentials from the keyring, Hikvision sub stream.
        s.kind = Kind::Stream;
        s.addr = QStringLiteral("rtsp://10.0.0.9:554/Streaming/Channels/101");
        s.username.clear();
        s.password.clear();
        QCOMPARE(liveUrl(s, true), s.addr);
        QCOMPARE(liveUrl(s, false), QStringLiteral("rtsp://10.0.0.9:554/Streaming/Channels/102"));
        s.username = QStringLiteral("u");
        s.password = QStringLiteral("p@ss");
        QCOMPARE(liveUrl(s, true),
                 QStringLiteral("rtsp://u:p%40ss@10.0.0.9:554/Streaming/Channels/101"));
    }
};

QTEST_APPLESS_MAIN(TestDeviceKind)
#include "test_devicekind.moc"
