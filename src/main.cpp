#include "core/CredentialStore.h"
#include "core/Database.h"
#include "core/LocalSeenStore.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/SingleInstance.h"
#include "core/Updater.h"
#include "ui/TrayIcon.h"
#include "device/DeviceDiscovery.h"
#include "device/BoschTourController.h"
#include "device/DeviceManager.h"
#include "device/DownloadQueue.h"
#include "device/Highlights.h"
#include "media/AlertSounds.h"
#include "device/EventManager.h"
#include "device/UnseenCounts.h"
#include "device/FrigateApi.h"
#include "media/StreamPlayer.h"
#include "media/LiveStream.h"
#include "media/PlaybackSession.h"
#include "media/ClipLoader.h"
#include "media/TalkSession.h"
#include "ui/LiveArrangement.h"
#include "ui/SiteMap.h"
#include "ui/SiteSelection.h"
#include "ui/SiteBudget.h"
#include "ui/SavedSelections.h"
#include "ui/PlaybackGroup.h"
#include "ui/LiveReplay.h"
#include "ui/EventCameraList.h"
#include "ui/EventDateBar.h"
#include "ui/CameraNoise.h"
#include "ui/EventClip.h"
#include "ui/PlaybackDay.h"
#include "ui/EventFeed.h"
#include "ui/FrameShape.h"
#include "ui/PaneBarRules.h"
#include "ui/FisheyeLens.h"
#include "ui/StillQueue.h"
#include "ui/MosaicLayout.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#include <chrono>
#include <thread>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

int main(int argc, char *argv[])
{
    // QApplication (not QGuiApplication): QSystemTrayIcon needs the widgets
    // layer for the tray icon + menu. The UI itself remains pure QML.
#ifdef __GLIBC__
    // Decoder threads allocate frames the GUI thread frees; with a private arena per thread
    // glibc keeps each arena's high-water mark and the process balloons (2.5 GB seen).
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
#endif
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("reolink-linux"));
    QCoreApplication::setApplicationName(QStringLiteral("reolink-client"));
    // Version comes from CMake (PROJECT_VERSION plus the fork tag, e.g. "0.2.0-jjr"),
    // so the source can't drift from what a build reports.
    QCoreApplication::setApplicationVersion(QStringLiteral(RL_APP_VERSION));

    // Give the app a real identity in the taskbar/dock. setDesktopFileName lets
    // Wayland compositors match the window to the installed .desktop file and
    // pull its icon (the fix for the generic fallback glyph); setWindowIcon
    // supplies the icon directly for X11 and environments without the install.
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.todesengelx.ReolinkLinux"));
    {
        QIcon icon;
        for (const int size : {16, 24, 32, 48, 64, 128, 256})
            icon.addFile(QStringLiteral(":/icons/reolink-%1.png").arg(size), QSize(size, size));
        if (!icon.isNull())
            QGuiApplication::setWindowIcon(icon);
    }

    rl::installLogging();

    // All Controls are custom-drawn against Theme.qml; Basic avoids
    // platform-style repaints fighting the dark palette.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    // --smoke <png>: load the UI, wait for first frames, grab the window to a
    // file and exit — headless end-to-end verification for CI and agents.
    QCommandLineOption smokeOption(QStringLiteral("smoke"),
                                   QStringLiteral("Capture a screenshot then exit."),
                                   QStringLiteral("png-path"));
    QCommandLineOption smokeDelayOption(QStringLiteral("smoke-delay"),
                                        QStringLiteral("Delay before capture (ms)."),
                                        QStringLiteral("ms"), QStringLiteral("3000"));
    // Headless recording verification: --record <source> --record-out <file>.
    QCommandLineOption recordOption(QStringLiteral("record"),
                                    QStringLiteral("Record a source then exit (no UI)."),
                                    QStringLiteral("source-url"));
    QCommandLineOption recordOutOption(QStringLiteral("record-out"),
                                       QStringLiteral("Output MP4 path for --record."),
                                       QStringLiteral("path"));
    QCommandLineOption recordSecsOption(QStringLiteral("record-secs"),
                                        QStringLiteral("Seconds to record."),
                                        QStringLiteral("s"), QStringLiteral("4"));
    QCommandLineOption discoverOption(QStringLiteral("discover"),
                                      QStringLiteral("Scan the LAN for Reolink devices then exit."));
    // Autostart entries launch with the window hidden — monitoring only.
    QCommandLineOption hiddenOption(QStringLiteral("start-hidden"),
                                    QStringLiteral("Start minimized to the system tray."));
    parser.addOption(smokeOption);
    parser.addOption(smokeDelayOption);
    parser.addOption(recordOption);
    parser.addOption(recordOutOption);
    parser.addOption(recordSecsOption);
    parser.addOption(discoverOption);
    parser.addOption(hiddenOption);
    parser.process(app);

    if (parser.isSet(discoverOption)) {
        auto *disc = new rl::DeviceDiscovery(&app);
        QObject::connect(disc, &rl::DeviceDiscovery::deviceFound, &app,
                         [](const QString &ip, const QString &info) {
                             qCInfo(lcUi) << "DISCOVERED Reolink device:" << ip << info;
                         });
        QObject::connect(disc, &rl::DeviceDiscovery::scanFinished, &app,
                         [] { qCInfo(lcUi) << "discovery finished"; QCoreApplication::exit(0); });
        disc->scan();
        QTimer::singleShot(30000, &app, [] { QCoreApplication::exit(1); });
        return app.exec();
    }

    if (parser.isSet(recordOption)) {
        auto *player = new rl::StreamPlayer(&app);
        const QString out = parser.value(recordOutOption);
        const int secs = parser.value(recordSecsOption).toInt();
        QObject::connect(player, &rl::StreamPlayer::stateChanged, &app, [player, out] {
            if (player->state() == rl::StreamPlayer::State::Streaming && !player->recording())
                player->startRecording(out);
        });
        QObject::connect(player, &rl::StreamPlayer::recordingSaved, &app, [](const QString &p) {
            qCInfo(lcUi) << "Recording saved:" << p;
            QCoreApplication::exit(0);
        });
        QObject::connect(player, &rl::StreamPlayer::recordingFailed, &app, [](const QString &e) {
            qCCritical(lcUi) << "Recording failed:" << e;
            QCoreApplication::exit(1);
        });
        player->setSource(parser.value(recordOption));
        player->start();
        QTimer::singleShot(secs * 1000, player, [player] { player->stopRecording(); });
        QTimer::singleShot((secs + 15) * 1000, &app, [] {
            qCCritical(lcUi) << "Recording timed out";
            QCoreApplication::exit(2);
        });
        return app.exec();
    }

    // One client per session. Closing the window leaves the app monitoring in
    // the tray, so relaunching from the launcher is routine — without this each
    // relaunch built a second app (own tray icon, NVR login, decoder set) and
    // they accumulated. Later launches raise the running window and exit.
    // Skipped for --smoke so headless capture stays independent of any running
    // client; RL_ALLOW_MULTI is the escape hatch for deliberately running two.
    rl::SingleInstance instance;
    const bool guardInstance = !parser.isSet(smokeOption)
                               && !qEnvironmentVariableIsSet("RL_ALLOW_MULTI");
    if (guardInstance && !instance.acquire(parser.isSet(hiddenOption))) {
        qCInfo(lcUi) << "another instance is already running; handed this launch to it";
        return 0;
    }

    rl::Database database(rl::Paths::databaseFile());
    if (!database.open()) {
        qCCritical(lcCore) << "Cannot open database:" << database.lastError();
        return 1;
    }
    rl::CredentialStore credentials;
    rl::DeviceManager devices(&database, &credentials);
    rl::EventManager events(&database, &devices);
    // Unseen counts (wayfinder/events-feed step 20): seen times kept on this desktop.
    rl::LocalSeenStore seenStore(&database);
    rl::UnseenCounts unseen;
    unseen.setStore(&seenStore);
    unseen.setEvents(&events);
    rl::Highlights highlights(&devices);
    rl::AlertSounds alertSounds;
    QObject::connect(&highlights, &rl::Highlights::soundRequested, &alertSounds,
                     [&alertSounds](const QString &sound) {
                         alertSounds.play(sound, rl::AlertSounds::Detection);
                     });
    rl::DeviceDiscovery discovery;
    rl::FisheyeAims fisheyeAims; // each camera's fisheye aim, shared by every page (playback-controls 13)
    rl::PaneBarRules paneBarRules;   // every video pane's bottom bar (live-replay ticket 11)
    rl::FrameShape frameShape;   // picture-shaped frames for QML (fisheye-and-framing step 06)
    rl::BoschTourController boschTour(&credentials);
    rl::Updater updater;
    rl::DownloadQueue downloads([&devices](int row) { return devices.downloadSource(row); });
    rl::TrayIcon tray;

    qmlRegisterType<rl::StreamPlayer>("ReolinkApp.Core", 1, 0, "StreamPlayer");
    qmlRegisterType<rl::LiveStream>("ReolinkApp.Core", 1, 0, "LiveStream");
    qmlRegisterType<rl::PlaybackSession>("ReolinkApp.Core", 1, 0, "PlaybackSession");
    qmlRegisterType<rl::ClipLoader>("ReolinkApp.Core", 1, 0, "ClipLoader");
    rl::frigate::installMediaAuth(); // Frigate Playback signs in with the session cookie
    qmlRegisterType<rl::TalkSession>("ReolinkApp.Core", 1, 0, "TalkSession");
    qmlRegisterType<rl::LiveArrangement>("ReolinkApp.Core", 1, 0, "LiveArrangement");
    qmlRegisterType<rl::SiteMap>("ReolinkApp.Core", 1, 0, "SiteMap");
    qmlRegisterType<rl::SiteSelection>("ReolinkApp.Core", 1, 0, "SiteSelection");
    qmlRegisterType<rl::SiteBudget>("ReolinkApp.Core", 1, 0, "SiteBudget");
    qmlRegisterType<rl::SavedSelections>("ReolinkApp.Core", 1, 0, "SavedSelections");
    qmlRegisterType<rl::PlaybackGroup>("ReolinkApp.Core", 1, 0, "PlaybackGroup");
    qmlRegisterType<rl::LiveReplay>("ReolinkApp.Core", 1, 0, "LiveReplay");
    qmlRegisterType<rl::EventFeed>("ReolinkApp.Core", 1, 0, "EventFeed");
    qmlRegisterType<rl::EventCameraList>("ReolinkApp.Core", 1, 0, "EventCameraList");
    qmlRegisterType<rl::EventDateBar>("ReolinkApp.Core", 1, 0, "EventDateBar");
    qmlRegisterType<rl::CameraNoise>("ReolinkApp.Core", 1, 0, "CameraNoise");
    qmlRegisterType<rl::EventClip>("ReolinkApp.Core", 1, 0, "EventClip");
    qmlRegisterType<rl::PlaybackDay>("ReolinkApp.Core", 1, 0, "PlaybackDay");
    qmlRegisterType<rl::MosaicLayout>("ReolinkApp.Core", 1, 0, "MosaicLayout");
    qmlRegisterType<rl::StillQueue>("ReolinkApp.Core", 1, 0, "StillQueue");
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "FrameShape", &frameShape);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "PaneBarRules", &paneBarRules);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "FisheyeAims", &fisheyeAims);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Devices", &devices);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "BoschTour", &boschTour);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Events", &events);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Unseen", &unseen);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Highlights", &highlights);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "AlertSounds", &alertSounds);
    // RL_MOCK_DETECTIONS="person:1:3@2;motion:1:4@4" plays scripted detections into Highlights.
    if (qEnvironmentVariableIsSet("RL_MOCK_DETECTIONS"))
        highlights.runScript(qEnvironmentVariable("RL_MOCK_DETECTIONS"));
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Discovery", &discovery);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Updater", &updater);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Downloads", &downloads);
    qmlRegisterSingletonInstance("ReolinkApp.Core", 1, 0, "Tray", &tray);

    QQmlApplicationEngine engine;
    // Lets tests/screenshots open a specific page (0=Live,1=Playback,2=Events,3=Settings).
    engine.rootContext()->setContextProperty(
        QStringLiteral("initialPage"), qEnvironmentVariableIntValue("RL_INITIAL_PAGE"));
    // RL_EVENTS_AUTOSCROLL=1 scrolls the Events feed to its end, logs the longest stall
    // between scroll steps, and quits: for checking the feed scrolls smoothly.
    engine.rootContext()->setContextProperty(
        QStringLiteral("eventsAutoScroll"), qEnvironmentVariableIsSet("RL_EVENTS_AUTOSCROLL"));
    // RL_EVENTS_PLAY=1 plays the shown camera's newest event in the Events clip player once
    // the feed has loaded: for checking the player without a click.
    engine.rootContext()->setContextProperty(
        QStringLiteral("eventsAutoPlay"), qEnvironmentVariableIsSet("RL_EVENTS_PLAY"));
    // RL_EVENTS_KEYS="J,J,K": with RL_EVENTS_PLAY, those keys go to the clip player 4 s after
    // its first picture (live-replay ticket 12): for checking speeds and reverse headless.
    // Steps, comma-separated: a key (J K L Left Right), "SD" (HD off), "KEEP" (Keep playing,
    // only once the clip has ended, as on screen) or "+N" (wait N seconds). From the first
    // step on, the clip's phase, speed and shown time are logged each second.
    engine.rootContext()->setContextProperty(
        QStringLiteral("eventsKeys"), qEnvironmentVariable("RL_EVENTS_KEYS"));
    // RL_SETTINGS_CATEGORY=<key> opens Settings on a section (e.g. "highlights", "mosaics").
    engine.rootContext()->setContextProperty(
        QStringLiteral("initialSettingsCategory"), qEnvironmentVariable("RL_SETTINGS_CATEGORY"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("mockRecordings"), qEnvironmentVariableIsSet("RL_MOCK_RECORDINGS"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("mockDoorbell"), qEnvironmentVariableIsSet("RL_MOCK_DOORBELL"));
    // RL_MOCK_DOORBELL=<hostId>:<channel>[@<secs>] points the mock at a real camera row
    // (and rings it that many seconds after start).
    engine.rootContext()->setContextProperty(
        QStringLiteral("mockDoorbellTarget"), qEnvironmentVariable("RL_MOCK_DOORBELL"));
    // RL_OPEN_CLIP=<hostId>:<channel> opens the detection clip for "30 s ago" at startup.
    engine.rootContext()->setContextProperty(
        QStringLiteral("openClipTarget"), qEnvironmentVariable("RL_OPEN_CLIP"));
    engine.rootContext()->setContextProperty(
        QStringLiteral("playbackAutoplay"), qEnvironmentVariableIsSet("RL_PLAYBACK_AUTOPLAY"));
    // RL_PLAYBACK_STOP_MS=<ms>: with RL_PLAYBACK_AUTOPLAY, press Stop that long after it starts.
    engine.rootContext()->setContextProperty(
        QStringLiteral("playbackStopMs"), qEnvironmentVariableIntValue("RL_PLAYBACK_STOP_MS"));
    // RL_PLAYBACK_AT=<secs into the day>: with RL_PLAYBACK_AUTOPLAY, play from there instead.
    engine.rootContext()->setContextProperty(
        QStringLiteral("playbackAtSecs"),
        qEnvironmentVariableIsSet("RL_PLAYBACK_AT") ? qEnvironmentVariableIntValue("RL_PLAYBACK_AT") : -1);
    // RL_PLAYBACK_HD=1: with RL_PLAYBACK_AUTOPLAY, switch the selected pane to HD first.
    engine.rootContext()->setContextProperty(
        QStringLiteral("playbackHd"), qEnvironmentVariableIsSet("RL_PLAYBACK_HD"));
    // RL_PLAYBACK_BACK=<n>: with RL_PLAYBACK_AUTOPLAY, play backwards: 1 = ◀ 1×, n > 1 = « n-1 times.
    engine.rootContext()->setContextProperty(
        QStringLiteral("playbackBackSteps"), qEnvironmentVariableIntValue("RL_PLAYBACK_BACK"));
    // RL_PLAYBACK_FAST=<n>: with RL_PLAYBACK_AUTOPLAY, press » (fast forward) n times once it plays.
    engine.rootContext()->setContextProperty(
        QStringLiteral("playbackFastSteps"), qEnvironmentVariableIntValue("RL_PLAYBACK_FAST"));
    // RL_PANE_BARS=1 shows every video pane's bottom bar without hover (live-replay ticket 11):
    // for screenshots, which have no pointer.
    engine.rootContext()->setContextProperty(
        QStringLiteral("paneBarsShown"), qEnvironmentVariableIsSet("RL_PANE_BARS"));
    // RL_LIVE_REPLAY="<hostId:channel>;<action>": Live View maximises that camera and, 8 s
    // later, starts Live replay: action "J" (◀ from now) or "<secs behind now>:<1|-1>"
    // (play from there forwards or backwards), optionally ":<» presses>" (live-replay 07);
    // optionally ";<secs>": ● Live that many seconds after replay starts (live-replay 08d);
    // optionally ";<key>,<key>": with the grid not maximised, those panes press J 2 s after
    // replay starts — joining it (live-replay 15b; "<secs>" may be empty); optionally
    // ";<key>@<secs>": maximise that camera then, and back to the grid 5 s later.
    engine.rootContext()->setContextProperty(
        QStringLiteral("liveReplayTest"), qEnvironmentVariable("RL_LIVE_REPLAY"));
    engine.rootContext()->setContextProperty(QStringLiteral("startHidden"),
                                             parser.isSet(hiddenOption) && tray.available());
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    // The app-wide QML singletons that load stored state, created before any page binds to
    // them: a first load inside a binding changes what that binding reads (a binding loop).
    for (const char *name : {"Sites", "PanePrefs", "Saved"})
        engine.singletonInstance<QObject *>("ReolinkApp", name);
    engine.loadFromModule("ReolinkApp", "Main");

    // RL_SCREENSHOT=<file.png> saves the window after RL_SCREENSHOT_MS (default 10 s) and
    // quits. For checking how the real UI looks without a person at the screen.
    if (qEnvironmentVariableIsSet("RL_SCREENSHOT")) {
        const int delayMs = qEnvironmentVariableIsSet("RL_SCREENSHOT_MS")
                                ? qEnvironmentVariableIntValue("RL_SCREENSHOT_MS") : 10000;
        QTimer::singleShot(delayMs, &app, [&engine, &app] {
            const QList<QObject *> roots = engine.rootObjects();
            if (!roots.isEmpty())
                if (auto *w = qobject_cast<QQuickWindow *>(roots.first()))
                    w->grabWindow().save(qEnvironmentVariable("RL_SCREENSHOT"));
            app.quit();
        });
    }

    // Tray -> window plumbing (QML handles close-to-tray via the Tray singleton).
    QObject::connect(&tray, &rl::TrayIcon::quitRequested, &app, [&app] {
        // Quit must be final: a worker blocked on an NVR socket can stall
        // teardown (the destructors join them), leaving a process with no tray
        // icon and no window. Arm an independent watchdog so "Quit" always
        // exits; SQLite/keyring writes are synchronous, so nothing is lost.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            // Reached only when graceful teardown stalled — worth knowing.
            qCWarning(lcUi) << "shutdown stalled; forcing exit";
            ::_exit(0);
        }).detach();
        app.quit();
    });
    auto raiseWindow = [&engine] {
        const QList<QObject *> roots = engine.rootObjects();
        if (roots.isEmpty())
            return;
        if (auto *w = qobject_cast<QQuickWindow *>(roots.first())) {
            w->show();
            w->raise();
            w->requestActivate();
        }
    };
    QObject::connect(&tray, &rl::TrayIcon::openRequested, &app, raiseWindow);
    // A second launch (launcher click, notification) reaches us here instead of
    // starting its own app; it forwarded its activation token so this raise is
    // allowed rather than suppressed as focus stealing.
    QObject::connect(&instance, &rl::SingleInstance::activationRequested, &app, raiseWindow);
    // The tray shows the unseen total across every camera (it was events since startup).
    QObject::connect(&unseen, &rl::UnseenCounts::changed, &tray,
                     [&tray, &unseen] { tray.setUnread(unseen.total()); });
    tray.setUnread(unseen.total());
    // With the window closed the app lives in the tray; don't exit.
    app.setQuitOnLastWindowClosed(!tray.available());

    // Check GitHub for a newer release shortly after the UI is up (skipped in
    // the headless screenshot mode). Silent unless an update is found.
    if (!parser.isSet(smokeOption))
        QTimer::singleShot(3000, &updater, &rl::Updater::check);

    // TEST HOOK (local): RL_TEST_NOTIFY=1 fires a synthetic detection through
    // the full pipeline (event inbox -> desktop notification -> click-to-raise)
    // so the notification flow can be exercised without waiting for real motion.
    if (qEnvironmentVariableIsSet("RL_TEST_NOTIFY")) {
        QTimer::singleShot(6000, &devices, [&devices] {
            if (devices.rowCount() == 0)
                return;
            const QVariantMap c = devices.cameraInfo(0);
            // RL_TEST_NOTIFY_TYPE picks the detection type (default person), so a
            // switched-off pair can be checked to go nowhere.
            const QString type =
                qEnvironmentVariable("RL_TEST_NOTIFY_TYPE", QStringLiteral("person"));
            devices.reportDetection(c.value(QStringLiteral("hostId")).toLongLong(),
                                    c.value(QStringLiteral("channel")).toInt(), type,
                                    c.value(QStringLiteral("name")).toString());
        });
    }

    if (parser.isSet(smokeOption)) {
        const QString outPath = parser.value(smokeOption);
        const int delayMs = parser.value(smokeDelayOption).toInt();
        QTimer::singleShot(delayMs, &app, [&engine, outPath] {
            const QList<QObject *> roots = engine.rootObjects();
            int exitCode = 1;
            if (!roots.isEmpty()) {
                if (auto *window = qobject_cast<QQuickWindow *>(roots.first())) {
                    const QImage shot = window->grabWindow();
                    if (!shot.isNull() && shot.save(outPath)) {
                        qCInfo(lcUi) << "Smoke screenshot saved to" << outPath;
                        exitCode = 0;
                    }
                }
            }
            QCoreApplication::exit(exitCode);
        });
    }

#ifdef __GLIBC__
    // Hand freed heap back to the OS periodically; the decode path churns large buffers.
    QTimer trimTimer;
    trimTimer.setInterval(60 * 1000);
    QObject::connect(&trimTimer, &QTimer::timeout, [] { malloc_trim(0); });
    trimTimer.start();
#endif
    return app.exec();
}
