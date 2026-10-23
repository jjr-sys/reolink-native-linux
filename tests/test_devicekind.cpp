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
        QTest::newRow("camera") << int(Kind::Camera) << true << false << false << false
                                << int(DetectionSource::ReolinkPollAndPush);
        QTest::newRow("nvr") << int(Kind::Nvr) << true << true << false << false
                             << int(DetectionSource::ReolinkPollAndPush);
        QTest::newRow("stream") << int(Kind::Stream) << false << false << true << false
                                << int(DetectionSource::None);
        QTest::newRow("frigate") << int(Kind::Frigate) << false << true << false << true
                                 << int(DetectionSource::FrigateServer);
    }
    void capabilityTable()
    {
        QFETCH(int, kind);
        QFETCH(bool, reolink);
        QFETCH(bool, multiChannel);
        QFETCH(bool, alwaysOnline);
        QFETCH(bool, frigate);
        QFETCH(int, detections);
        const Capabilities c = capabilities(Kind(kind));
        QCOMPARE(c.reolinkProtocol, reolink);
        QCOMPARE(c.direct(), !reolink);
        QCOMPARE(c.multiChannel, multiChannel);
        QCOMPARE(c.alwaysOnline, alwaysOnline);
        QCOMPARE(c.frigateApi, frigate);
        QCOMPARE(int(c.detections), detections);
    }
    void capabilityMapForQml()
    {
        const QVariantMap m = capabilityMap(Kind::Frigate);
        QCOMPARE(m.value("multiChannel").toBool(), true);
        QCOMPARE(m.value("direct").toBool(), true);
        QCOMPARE(m.value("frigateApi").toBool(), true);
        QCOMPARE(m.value("reolinkProtocol").toBool(), false);
    }

    void hikvisionIsRecognisedByPath()
    {
        QCOMPARE(streamVendor(QStringLiteral("rtsp://h:554/Streaming/Channels/101")),
                 StreamVendor::Hikvision);
        QCOMPARE(streamVendor(QStringLiteral("rtsp://h:554/h264Preview_01_main")),
                 StreamVendor::Generic);
    }
    void boschFeedIsRecognisedByName()
    {
        QVERIFY(looksLikeBoschFeed(QStringLiteral("Bosch Camera")));
        QVERIFY(looksLikeBoschFeed(QStringLiteral("front BOSCH")));
        QVERIFY(!looksLikeBoschFeed(QStringLiteral("Driveway")));
        QVERIFY(!looksLikeBoschFeed(QString()));
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
