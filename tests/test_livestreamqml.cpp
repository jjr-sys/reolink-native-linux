#include "core/Database.h"
#include "device/BoschTourController.h"
#include "media/LiveStream.h"
#include "media/PlaybackSession.h"
#include "media/TalkSession.h"
#include "ui/FisheyeLens.h"
#include "ui/LiveReplay.h"
#include "ui/PaneBarRules.h"
#include "ui/PictureSurface.h"
#include "ui/TestHooks.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;

class TestLiveStreamQml : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    CredentialStore m_credentials;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<DeviceManager> m_devices;
    std::unique_ptr<BoschTourController> m_bosch;
    rl::FisheyeAims m_aims;
    rl::PaneBarRules m_paneBar;
    rl::TestHooks m_testHooks;
    std::unique_ptr<QQmlEngine> m_engine;
    qint64 m_host = 0;

private slots:
    void initTestCase()
    {
        QQuickStyle::setStyle("Basic");
        QCoreApplication::setOrganizationName("reolink-livestream-qml-test");
        QCoreApplication::setApplicationName("livestream-qml-test");
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        m_db = std::make_unique<Database>(m_dir.filePath("devices.db"), "livestream_qml");
        QVERIFY(m_db->open());
        HostRecord host;
        host.kind = "stream";
        host.name = "Local test camera";
        host.addr = m_dir.filePath("missing.mp4");
        m_host = m_db->addHost(host);
        QVERIFY(m_host > 0);
        m_devices = std::make_unique<DeviceManager>(m_db.get(), &m_credentials);
        m_bosch = std::make_unique<BoschTourController>(&m_credentials);
        qmlRegisterType<LiveStream>("ReolinkApp.Core", 1, 0, "LiveStream");
        qmlRegisterType<StreamPlayer>("ReolinkApp.Core", 1, 0, "StreamPlayer");
        qmlRegisterType<TalkSession>("ReolinkApp.Core", 1, 0, "TalkSession");
        // A Live pane carries Live replay, which plays through Playback's pane (ticket 07).
        qmlRegisterType<PlaybackSession>("ReolinkApp.Core", 1, 0, "PlaybackSession");
        qmlRegisterType<PlaybackGroup>("ReolinkApp.Core", 1, 0, "PlaybackGroup");
        qmlRegisterType<LiveReplay>("ReolinkApp.Core", 1, 0, "LiveReplay");
        qmlRegisterType<PictureSurface>("ReolinkApp.Core", 1, 0, "PictureSurface");
        qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Devices", m_devices.get());
        qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "BoschTour", m_bosch.get());
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(RL_QML_SOURCE_DIR "/Theme.qml")),
                                 "ReolinkApp", 1, 0, "Theme");
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(RL_QML_SOURCE_DIR "/FisheyeViews.qml")),
                                 "ReolinkApp", 1, 0, "FisheyeViews");
        qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "FisheyeAims", &m_aims);
        // Every video pane's bottom bar (live-replay ticket 11) and its sound button.
        qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "PaneBarRules", &m_paneBar);
        qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "TestHooks", &m_testHooks);
        qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(RL_QML_SOURCE_DIR "/AudioPrefs.qml")),
                                 "ReolinkApp", 1, 0, "AudioPrefs");
        m_engine = std::make_unique<QQmlEngine>();
        // The executable's build directory contains the app's qmldir, whose
        // preferred URLs refer to resources embedded only in the app binary.
        auto imports = m_engine->importPathList();
        imports.removeAll(QCoreApplication::applicationDirPath());
        m_engine->setImportPathList(imports);
    }

    void gridAndPopoutUseSharedLifecycle_data()
    {
        QTest::addColumn<QString>("file");
        QTest::newRow("grid") << "LivePane.qml";
        QTest::newRow("popout") << "PoppedWindow.qml";
        QTest::newRow("doorbell") << "DoorbellOverlay.qml";
    }

    void gridAndPopoutUseSharedLifecycle()
    {
        QFETCH(QString, file);
        QStringList warnings;
        const auto connection = connect(m_engine.get(), &QQmlEngine::warnings, this,
                                        [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings.append(error.toString());
        });
        QQmlComponent component(m_engine.get(), QUrl::fromLocalFile(
            QStringLiteral(RL_QML_SOURCE_DIR "/components/") + file));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QVariantMap properties{{"cameraIdentity", QVariantMap{{"hostId", m_host}, {"channel", 0}}},
                               {"label", "Local camera"}, {"width", 640}, {"height", 360}};
        if (file == "LivePane.qml")
            properties.insert("deviceRow", 0);
        if (file == "DoorbellOverlay.qml") {
            properties.remove("label");
            properties.insert("camera", "Local camera");
            properties.insert("deviceRow", 0);
            properties.insert("active", true);
        }
        std::unique_ptr<QObject> view(component.createWithInitialProperties(properties));
        QVERIFY2(view, qPrintable(component.errorString()));
        auto *stream = view->findChild<LiveStream *>();
        QVERIFY(stream);
        QTRY_COMPARE(stream->state(), StreamPlayer::State::Stopped);
        // A local URL failure proves the transport was attempted. The old pop-out
        // instead silently tried Baichuan and remained unstarted for direct cameras.
        QVERIFY(!stream->errorString().isEmpty());
        QVERIFY(!stream->degraded());
        if (auto *window = qobject_cast<QQuickWindow *>(view.get())) {
            window->close();
        } else if (file == "DoorbellOverlay.qml") {
            view->setProperty("active", false);
        } else {
            view->setProperty("pageActive", false);
        }
        QCOMPARE(stream->state(), StreamPlayer::State::Stopped);
        QVERIFY(!stream->recovering());
        view.reset();
        QCoreApplication::processEvents();
        disconnect(connection);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    // bosch-ptz-port ticket 04: the Bosch drag surface reaches BoschTour (ReolinkApp.Core);
    // without it every press was ignored and an empty status chip showed as a black dot.
    void boschSteerSurfaceLoadsWithoutWarnings()
    {
        QStringList warnings;
        const auto connection = connect(m_engine.get(), &QQmlEngine::warnings, this,
                                        [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings.append(error.toString());
        });
        QQmlComponent component(m_engine.get(), QUrl::fromLocalFile(
            QStringLiteral(RL_QML_SOURCE_DIR "/components/BoschSteerSurface.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> surface(component.createWithInitialProperties(
            QVariantMap{{"width", 640}, {"height", 360}}));
        QVERIFY2(surface, qPrintable(component.errorString()));
        QCOMPARE(surface->property("steerable").metaType().id(), QMetaType::Bool);
        QCOMPARE(surface->property("steerable").toBool(), false);   // HA not connected here
        bool chipShown = false;
        for (auto *item : surface->findChildren<QQuickItem *>())
            if (item->property("color").isValid() && item->property("radius").isValid()
                && item->childItems().size() == 1 && item->isVisible()
                && qobject_cast<QQuickItem *>(item->childItems().first())
                && item->y() == 8)
                chipShown = true;
        QVERIFY(!chipShown);   // no tour status: no chip
        surface.reset();
        QCoreApplication::processEvents();
        disconnect(connection);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    // bosch-ptz-port ticket 03: the Tracking ⌃ panel reads BoschTour (ReolinkApp.Core); with
    // Home Assistant not connected it says the service is unavailable.
    void boschTrackingPanelLoadsWithoutWarnings()
    {
        QStringList warnings;
        const auto connection = connect(m_engine.get(), &QQmlEngine::warnings, this,
                                        [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings.append(error.toString());
        });
        QQmlComponent component(m_engine.get(), QUrl::fromLocalFile(
            QStringLiteral(RL_QML_SOURCE_DIR "/components/BoschTrackingPanel.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> panel(component.createWithInitialProperties(
            QVariantMap{{"width", 280}}));
        QVERIFY2(panel, qPrintable(component.errorString()));
        QCOMPARE(panel->property("known").metaType().id(), QMetaType::Bool);
        QCOMPARE(panel->property("known").toBool(), false);
        QCOMPARE(panel->property("statusText").toString(), QStringLiteral("Status unavailable"));
        panel.reset();
        QCoreApplication::processEvents();
        disconnect(connection);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void cleanupTestCase()
    {
        m_engine.reset();
        m_bosch.reset();
        m_devices.reset();
        m_db.reset();
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    TestLiveStreamQml test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_livestreamqml.moc"
