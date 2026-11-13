#include "ui/FisheyeLens.h"
#include "ui/FrameShape.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace rl;

namespace {
constexpr double kDeg = M_PI / 180;
}

// Fisheye views shared by Live, Playback and Events (wayfinder/playback-controls ticket 13): the
// tiles of each view, how dragging and scrolling aim them, the shape each view is drawn at, and
// the aim remembered per camera and view.
class TestFisheyeLens : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
        QCoreApplication::setOrganizationName(QStringLiteral("rl-test-fisheyelens"));
        QCoreApplication::setApplicationName(QStringLiteral("rl-test-fisheyelens"));
    }

    // ---- shapes (the table itself is tested in test_frameshape) ---------------------------

    void everyViewFitsItsOwnShapeWithoutGaps()
    {
        for (const QString &view : fisheye::views()) {
            const double h = 900, w = h * frameshape::viewRatio(view);
            const auto tiles = fisheye::tiles(view, w, h);
            QVERIFY2(!tiles.isEmpty(), qPrintable(view));
            double area = 0;
            for (const auto &t : tiles) {
                QVERIFY2(t.w > 0 && t.h > 0, qPrintable(view));
                QVERIFY2(t.x >= -0.5 && t.y >= -0.5 && t.x + t.w <= w + 0.5 && t.y + t.h <= h + 0.5, qPrintable(view));
                area += t.w * t.h;
            }
            // Gaps of 2 px between tiles only: at its own shape a view fills its pane.
            QVERIFY2(area > 0.98 * w * h, qPrintable(view + QStringLiteral(" fills ") + QString::number(area / (w * h))));
        }
    }

    // ---- tiles ---------------------------------------------------------------------------

    void theQuadIsFourWideViewsInTwoRows()
    {
        const auto tiles = fisheye::tiles(QStringLiteral("quad"), 1600, 900, 2);
        QCOMPARE(tiles.size(), 4);
        QCOMPARE(tiles[1].x + tiles[1].w, 1600.0);
        QCOMPARE(tiles[3].y + tiles[3].h, 900.0);
        QCOMPARE(tiles[1].x, tiles[0].w + 2);
        for (int k = 0; k < 4; ++k) {
            QCOMPARE(tiles[k].kind, fisheye::Tile::Ptz);
            QVERIFY(std::abs(tiles[k].w / tiles[k].h - 16.0 / 9) < 0.01);
            QVERIFY(std::abs(tiles[k].pan - k * 90 * kDeg) < 1e-9);
            QCOMPARE(tiles[k].fov, fisheye::kQuadFov);
            QCOMPARE(tiles[k].tilt, fisheye::kQuadTilt);
        }
    }

    // Whole-circle coverage: every direction the lens sees, from straight down out to 88°,
    // lands in one of the four 16:9 views (the old 85° views left wedges at the rim, and
    // Outpost's 110°/36° does too once its tiles are 16:9).
    void theQuadSeesTheWholeCircle()
    {
        const auto tiles = fisheye::tiles(QStringLiteral("quad"), 1600, 900, 0);
        constexpr int N = 40;   // grid cells per lens radius
        QList<QList<bool>> seen(2 * N + 1, QList<bool>(2 * N + 1, false));
        for (const auto &t : tiles)
            for (int i = 0; i <= 200; ++i)
                for (int j = 0; j <= 200; ++j) {
                    const auto p = fisheye::sample(t, i / 200.0, j / 200.0);
                    if (!p)
                        continue;
                    const int gx = int((p->x() - fisheye::kLensCx) / fisheye::kLensRadius * N + N);
                    const int gy = int((p->y() - fisheye::kLensCy) / fisheye::kLensRadius * N + N);
                    if (gx >= 0 && gx <= 2 * N && gy >= 0 && gy <= 2 * N)
                        seen[gx][gy] = true;
                }
        const double limit = N * 88.0 / 90 - 1.5;
        for (int gx = 0; gx <= 2 * N; ++gx)
            for (int gy = 0; gy <= 2 * N; ++gy)
                if (std::hypot(gx - N, gy - N) <= limit)
                    QVERIFY2(seen[gx][gy], qPrintable(QStringLiteral("unseen at %1,%2").arg(gx).arg(gy)));
    }

    // No Quad view shows sky (the lens sees nothing above the horizon, so that would be black),
    // in its own 16:9 pane or in a maximised pane of another shape.
    void theQuadShowsNothingAboveTheHorizon_data()
    {
        QTest::addColumn<double>("w");
        QTest::addColumn<double>("h");
        QTest::newRow("16:9") << 1600.0 << 900.0;
        QTest::newRow("square") << 1000.0 << 1000.0;
        QTest::newRow("tall") << 600.0 << 1000.0;
    }
    void theQuadShowsNothingAboveTheHorizon()
    {
        QFETCH(double, w);
        QFETCH(double, h);
        const auto tiles = fisheye::tiles(QStringLiteral("quad"), w, h, 0);
        for (const auto &t : tiles)
            for (int i = 0; i <= 20; ++i)
                QVERIFY(fisheye::sample(t, i / 20.0, 0.0).has_value());
    }

    void fisheyePlusPtzIsTheCircleBesideAFourByThreeCamera()
    {
        const auto tiles = fisheye::tiles(QStringLiteral("fisheye_ptz"), 700, 300, 2);
        QCOMPARE(tiles.size(), 2);
        QCOMPARE(tiles[0].kind, fisheye::Tile::Raw);
        QCOMPARE(tiles[0].w, tiles[0].h);
        QCOMPARE(tiles[0].h, 300.0);
        QCOMPARE(tiles[1].kind, fisheye::Tile::Ptz);
        QCOMPARE(tiles[1].x + tiles[1].w, 700.0);
        QVERIFY(std::abs(tiles[1].w / tiles[1].h - 4.0 / 3) < 0.02);
    }

    void dualIsTwoHalvesStacked()
    {
        const auto tiles = fisheye::tiles(QStringLiteral("dual"), 1050, 1000, 2);
        QCOMPARE(tiles.size(), 2);
        QCOMPARE(tiles[0].kind, fisheye::Tile::Pano);
        QVERIFY(std::abs(tiles[0].span - M_PI) < 1e-9);
        QVERIFY(std::abs(tiles[1].phi0 - M_PI) < 1e-9);
        QVERIFY(tiles[1].y >= tiles[0].y + tiles[0].h);
    }

    // In a pane of another shape (a fixed Live grid cell) the strips keep their own shape, centred.
    void aPanoramaInAWidePaneKeepsItsShapeCentred()
    {
        const auto tiles = fisheye::tiles(QStringLiteral("panorama"), 1600, 900, 2);
        QCOMPARE(tiles.size(), 1);
        QVERIFY(std::abs(tiles[0].w / tiles[0].h - 3.3) < 0.01);
        QVERIFY(std::abs(tiles[0].x + tiles[0].w / 2 - 800) < 0.5);
        QVERIFY(std::abs(tiles[0].y + tiles[0].h / 2 - 450) < 0.5);
    }

    void straightDownIsTheCircleCentre()
    {
        fisheye::Tile down;
        down.kind = fisheye::Tile::Ptz;
        down.w = down.h = 100;
        down.tilt = 1e-6;
        down.fov = M_PI / 2;
        const auto p = fisheye::sample(down, 0.5, 0.5);
        QVERIFY(p);
        QVERIFY(std::abs(p->x() - fisheye::kLensCx) < 1e-4);
        QVERIFY(std::abs(p->y() - fisheye::kLensCy) < 1e-4);
    }

    void aPointFindsItsTile()
    {
        const auto tiles = fisheye::tiles(QStringLiteral("quad"), 1600, 900, 4);
        QCOMPARE(fisheye::at(tiles, 10, 10), 0);
        QCOMPARE(fisheye::at(tiles, 1590, 890), 3);
        QCOMPARE(fisheye::at(tiles, 799, 10), -1);   // in the gap
    }

    // ---- aiming --------------------------------------------------------------------------

    void draggingAcrossAViewTurnsItByItsWidth()
    {
        const auto start = fisheye::tiles(QStringLiteral("ptz"), 400, 225).first();
        const auto turned = fisheye::look(start, start, 400, 0);
        QVERIFY(std::abs(turned.pan - (start.pan - start.fov)) < 1e-9);
        QCOMPARE(turned.tilt, start.tilt);
    }

    void aViewTiltsNoHigherThanTheHorizonNorLowerThanNearlyStraightDown()
    {
        const auto start = fisheye::tiles(QStringLiteral("ptz"), 400, 225).first();
        const auto up = fisheye::look(start, start, 0, 1e6);
        QCOMPARE(up.tilt, std::max(fisheye::maxTilt(start.fov, 225.0 / 400), start.tilt));
        const auto down = fisheye::look(start, start, 0, -1e6);
        QVERIFY(std::abs(down.tilt - 8 * kDeg) < 1e-9);
    }

    void zoomingStaysBetweenThirtyAndOneHundredTwentyDegrees()
    {
        const auto start = fisheye::tiles(QStringLiteral("ptz"), 400, 225).first();
        QVERIFY(std::abs(fisheye::widen(start, start, 100).fov - 30 * kDeg) < 1e-9);
        QVERIFY(std::abs(fisheye::widen(start, start, 0.01).fov - 120 * kDeg) < 1e-9);
        // Widening never lets the top edge pass the horizon.
        auto low = start;
        low.tilt = fisheye::maxTilt(start.fov, 225.0 / 400);
        const auto wide = fisheye::widen(low, start, 0.5);
        QVERIFY(wide.tilt <= std::max(fisheye::maxTilt(wide.fov, 225.0 / 400), start.tilt) + 1e-9);
    }

    void aPanoramaSlidesSideways()
    {
        const auto start = fisheye::tiles(QStringLiteral("panorama"), 3300, 1000, 0).first();
        QVERIFY(std::abs(fisheye::look(start, start, start.w / 2, 0).phi0 + start.span / 2) < 1e-9);
    }

    void theCircleCannotBeAimed()
    {
        const auto raw = fisheye::tiles(QStringLiteral("fisheye_ptz"), 700, 300).first();
        const auto moved = fisheye::look(raw, raw, 50, 50);
        QCOMPARE(moved.pan, raw.pan);
        QCOMPARE(moved.phi0, raw.phi0);
    }

    void anAimTurnsItsTileOnly()
    {
        fisheye::Aims aims(4);
        aims[1] = fisheye::Aim{1.0, 0.5, 1.2, 0};
        const auto quad = fisheye::tiles(QStringLiteral("quad"), 1600, 900, 2, aims);
        QCOMPARE(quad[1].pan, 1.0);
        QCOMPARE(quad[1].fov, 1.2);
        QCOMPARE(quad[0].tilt, fisheye::kQuadTilt);
    }

    void aimsAreWrittenAndReadBack()
    {
        fisheye::Aims aims(2);
        aims[1] = fisheye::Aim{1.25, -0.5, 0.75, 3.0};
        const QString text = fisheye::encodeAims(aims);
        const auto back = fisheye::decodeAims(text);
        QCOMPARE(back.size(), 2);
        QVERIFY(!back[0]);
        QVERIFY(back[1]);
        QCOMPARE(back[1]->pan, 1.25);
        QCOMPARE(back[1]->phi0, 3.0);
        QVERIFY(fisheye::decodeAims(QStringLiteral("nonsense;1,2")).size() == 2);
        QVERIFY(!fisheye::decodeAims(QStringLiteral("nonsense;1,2"))[0]);
    }

    // ---- the store: one aim per camera and view, shared by every page ---------------------

    void theStoreKeepsEachCameraAndViewApart()
    {
        QSettings().remove(QStringLiteral("fisheyeAim"));
        FisheyeAims store;
        QSignalSpy changed(&store, &FisheyeAims::changed);
        QVERIFY(!store.aimed(QStringLiteral("5:0"), QStringLiteral("quad")));
        store.setAim(QStringLiteral("5:0"), QStringLiteral("quad"), 2, fisheye::Aim{1, 0.7, 1.5, 0});
        QCOMPARE(changed.count(), 1);
        QVERIFY(store.aimed(QStringLiteral("5:0"), QStringLiteral("quad")));
        QVERIFY(!store.aimed(QStringLiteral("5:0"), QStringLiteral("ptz")));
        QVERIFY(!store.aimed(QStringLiteral("6:0"), QStringLiteral("quad")));
        const auto aims = store.aims(QStringLiteral("5:0"), QStringLiteral("quad"));
        QCOMPARE(aims.size(), 3);
        QVERIFY(!aims[0] && aims[2]);
        QCOMPARE(aims[2]->tilt, 0.7);

        // Not on disk while the pointer moves; flushed, a second store (the next run) reads it.
        QVERIFY(!QSettings().contains(QStringLiteral("fisheyeAim/5_0/quad")));
        store.flush();
        FisheyeAims again;
        QVERIFY(again.aimed(QStringLiteral("5:0"), QStringLiteral("quad")));
        QCOMPARE(again.aims(QStringLiteral("5:0"), QStringLiteral("quad"))[2]->fov, 1.5);
    }

    void resettingForgetsTheViewsAim()
    {
        FisheyeAims store;
        store.setAim(QStringLiteral("7:1"), QStringLiteral("ptz"), 0, fisheye::Aim{1, 0.7, 1.5, 0});
        store.setAim(QStringLiteral("7:1"), QStringLiteral("dual"), 1, fisheye::Aim{0, 0, 0, 2});
        store.flush();
        QSignalSpy changed(&store, &FisheyeAims::changed);
        store.reset(QStringLiteral("7:1"), QStringLiteral("ptz"));
        QCOMPARE(changed.count(), 1);
        QVERIFY(!store.aimed(QStringLiteral("7:1"), QStringLiteral("ptz")));
        QVERIFY(store.aimed(QStringLiteral("7:1"), QStringLiteral("dual")));
        store.reset(QStringLiteral("7:1"), QStringLiteral("ptz"));   // nothing to forget: no signal
        QCOMPARE(changed.count(), 1);
        FisheyeAims again;
        QVERIFY(!again.aimed(QStringLiteral("7:1"), QStringLiteral("ptz")));
    }

    // What QML calls: tiles with the camera's aims, and a drag that lands in the store.
    void qmlDragsAimTheTileUnderThePointer()
    {
        QSettings().remove(QStringLiteral("fisheyeAim"));
        FisheyeAims store;
        const QString key = QStringLiteral("5:0"), view = QStringLiteral("quad");
        QVERIFY(store.drag(key, view, 1600, 900, 1500, 800, 100, 0));   // bottom-right quarter
        const auto aims = store.aims(key, view);
        QCOMPARE(aims.size(), 4);
        QVERIFY(!aims[0] && aims[3]);
        QVERIFY(aims[3]->pan < 270 * kDeg);
        QVERIFY(!store.drag(key, view, 1600, 900, 799, 10, 100, 0));     // the gap: nothing
        QVERIFY(store.zoom(key, view, 1600, 900, 10, 10, true));
        QVERIFY(store.aims(key, view)[0]->fov < fisheye::kQuadFov);
        const QVariantList tiles = store.tileList(key, view, 1600, 900);
        QCOMPARE(tiles.size(), 4);
        QCOMPARE(tiles[3].toMap().value(QStringLiteral("pan")).toDouble(), aims[3]->pan);
        QCOMPARE(tiles[0].toMap().value(QStringLiteral("kind")).toString(), QStringLiteral("ptz"));
        QVERIFY(!store.drag(key, QStringLiteral("fisheye"), 1000, 1000, 500, 500, 10, 10));
    }
};

QTEST_GUILESS_MAIN(TestFisheyeLens)
#include "test_fisheyelens.moc"
