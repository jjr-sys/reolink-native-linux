#include "ui/FisheyeLens.h"
#include "ui/PictureSurface.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;

// The picture rules of a video pane (architecture review, picture surface): zoom, pan limits, the
// drag threshold, the measured shape and the dewarp / circle decision. Everything goes through
// the interface the panes bind to; no QML.
class TestPictureSurface : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;   // the aim store writes QSettings: keep it off the real ones

    // A 1000 x 500 pane showing a 16:9 picture fitted into it (889 x 500, centred).
    static void showFlat(PictureSurface &s)
    {
        s.setViewport(QSizeF(1000, 500));
        s.setStreaming(true);
        s.setContentRect(QRectF(55.5, 0, 889, 500));
        s.setPictureRatio(16.0 / 9);
    }
    // A fisheye camera's circle on the same pane (500 x 500, centred).
    static void showCircle(PictureSurface &s, const QString &mode)
    {
        s.setViewport(QSizeF(1000, 500));
        s.setFisheyeLens(true);
        s.setFisheyeMode(mode);
        s.setStreaming(true);
        s.setPictureRatio(1.0);
        s.setContentRect(QRectF(250, 0, 500, 500));
    }

private slots:
    void initTestCase()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
        QCoreApplication::setOrganizationName(QStringLiteral("rl-test-picturesurface"));
        QCoreApplication::setApplicationName(QStringLiteral("rl-test-picturesurface"));
    }

    // ---- zoom and pan ---------------------------------------------------------------------
    void startsUnzoomed()
    {
        PictureSurface s;
        QCOMPARE(s.zoom(), 1.0);
        QCOMPARE(s.panX(), 0.0);
        QCOMPARE(s.panY(), 0.0);
        QVERIFY(!s.panning());
    }

    void wheelStepsAre15PercentWithin1And8()
    {
        PictureSurface s;
        showFlat(s);
        s.zoomStep(true);
        QCOMPARE(s.zoom(), 1.15);
        QVERIFY(s.panning());
        s.zoomStep(false);
        QVERIFY(qAbs(s.zoom() - 1.15 * 0.87) < 1e-12);   // out is 13 %, so not an exact undo
        s.zoomStep(false);
        QCOMPARE(s.zoom(), 1.0);          // never out past the whole picture
        for (int i = 0; i < 40; ++i)
            s.zoomStep(true);
        QCOMPARE(s.zoom(), 8.0);
    }

    void panStopsAtTheEdgesOfTheZoomedPicture()
    {
        PictureSurface s;
        showFlat(s);                      // picture 889 x 500 in 1000 x 500
        s.setZoom(2);
        s.panBy(5000, 5000);
        QCOMPARE(s.panX(), (889 * 2 - 1000) / 2.0);   // 389
        QCOMPARE(s.panY(), 250.0);                    // (500 * 2 - 500) / 2
        s.panBy(-5000, -5000);
        QCOMPARE(s.panX(), -389.0);
        QCOMPARE(s.panY(), -250.0);
    }

    void aPictureNarrowerThanItsViewportCannotPanSideways()
    {
        PictureSurface s;
        showFlat(s);
        s.setZoom(1.1);                   // 889 * 1.1 = 978 < 1000
        s.panBy(300, 0);
        QCOMPARE(s.panX(), 0.0);
    }

    void zoomingBackOutReelsThePanIn()
    {
        PictureSurface s;
        showFlat(s);
        s.setZoom(2);
        s.panBy(300, 200);
        s.zoomStep(false);                // 1.74: limit is (889 * 1.74 - 1000) / 2 = 273.5
        QVERIFY(s.panX() <= 889 * s.zoom() / 2 - 500 + 1e-9);
        QVERIFY(s.panX() > 0);
        while (s.zoom() > 1.0)
            s.zoomStep(false);
        QCOMPARE(s.panX(), 0.0);
        QCOMPARE(s.panY(), 0.0);
    }

    void theZoomButtonToggles()
    {
        PictureSurface s;
        showFlat(s);
        s.toggleZoom();
        QCOMPARE(s.zoom(), 2.0);
        s.panBy(100, 0);
        s.toggleZoom();
        QCOMPARE(s.zoom(), 1.0);
        QCOMPARE(s.panX(), 0.0);
        s.setZoom(1.005);                 // a hair over 1 still counts as unzoomed for the button
        s.toggleZoom();
        QCOMPARE(s.zoom(), 2.0);
    }

    void writingZoomAndPanDirectlyStillWorks()
    {
        // The pages set pbPane.zoom = 2 and zoom = 1; panX = 0 straight on the pane.
        PictureSurface s;
        showFlat(s);
        s.setZoom(2);
        QCOMPARE(s.zoom(), 2.0);
        s.setZoom(99);
        QCOMPARE(s.zoom(), 8.0);
        s.setZoom(0.2);
        QCOMPARE(s.zoom(), 1.0);
        s.setZoom(3);
        s.setPanX(40);
        s.setPanY(-30);
        QCOMPARE(s.panX(), 40.0);
        QCOMPARE(s.panY(), -30.0);
        s.resetZoom();
        QCOMPARE(s.zoom(), 1.0);
        QCOMPARE(s.panX(), 0.0);
        QCOMPARE(s.panY(), 0.0);
    }

    // ---- the drag threshold ---------------------------------------------------------------
    void aPressPicksThePaneUpPast8Pixels()
    {
        PictureSurface s;
        QCOMPARE(s.dragThreshold(), 8.0);
        QVERIFY(!s.pastDragThreshold(0, 0));
        QVERIFY(!s.pastDragThreshold(5, 5));        // 7.07
        QVERIFY(!s.pastDragThreshold(8, 0));        // exactly 8: not past
        QVERIFY(s.pastDragThreshold(6, 6));         // 8.49
        QVERIFY(s.pastDragThreshold(0, -9));
    }

    // ---- measuring the picture's shape ----------------------------------------------------
    void measuresTheDrawnPicture()
    {
        PictureSurface s;
        QCOMPARE(s.ratio(), 0.0);
        showFlat(s);
        QVERIFY(qAbs(s.ratio() - 889.0 / 500) < 1e-9);
    }

    void aTurnedStreamIsMeasuredTheOtherWayRound()
    {
        PictureSurface s;
        s.setRotation(90);
        s.setViewport(QSizeF(500, 1000));
        s.setStreaming(true);
        s.setContentRect(QRectF(0, 0, 281, 500));     // 16:9 turned: drawn 281 wide, 500 tall
        QVERIFY(qAbs(s.ratio() - 500.0 / 281) < 1e-9);
    }

    void aShapeMovesOnlyByMoreThan3Percent()
    {
        PictureSurface s;
        showFlat(s);
        const double first = s.ratio();
        s.setContentRect(QRectF(0, 0, 889 * 1.029, 500));   // a re-pack nudging the pane
        QCOMPARE(s.ratio(), first);
        s.setContentRect(QRectF(0, 0, 889 * 1.031, 500));
        QVERIFY(s.ratio() > first * 1.03);
    }

    void nothingIsMeasuredWithoutAPictureOnScreen()
    {
        PictureSurface s;
        s.setViewport(QSizeF(1000, 500));
        s.setContentRect(QRectF(0, 0, 800, 450));
        QCOMPARE(s.ratio(), 0.0);                    // not streaming
        s.setStreaming(true);
        s.setContentRect(QRectF(0, 0, 1, 1));        // nothing drawn yet
        QCOMPARE(s.ratio(), 0.0);
    }

    void aDewarpedPictureIsNotMeasured()
    {
        // The dewarp stretches the frame over the whole pane: that is not the picture's shape.
        PictureSurface s;
        showCircle(s, QStringLiteral("fisheye"));
        QCOMPARE(s.ratio(), 1.0);
        s.setFisheyeMode(QStringLiteral("quad"));
        QVERIFY(s.dewarp());
        s.setContentRect(QRectF(0, 0, 1000, 500));
        QCOMPARE(s.ratio(), 1.0);                    // kept from the circle
    }

    // ---- the dewarp rule (one rule for every pane) ----------------------------------------
    void aFisheyesCircleInAViewDewarps()
    {
        PictureSurface s;
        showCircle(s, QStringLiteral("quad"));
        QVERIFY(s.dewarp());
        QVERIFY(s.circle());
        QCOMPARE(s.shownRatio(), 16.0 / 9);
        s.setFisheyeMode(QStringLiteral("fisheye_ptz"));
        QCOMPARE(s.shownRatio(), 7.0 / 3);
        s.setFisheyeMode(QStringLiteral("panorama"));
        QCOMPARE(s.shownRatio(), 3.3);
    }

    void theRawCircleIsNotDewarped()
    {
        PictureSurface s;
        showCircle(s, QStringLiteral("fisheye"));
        QVERIFY(!s.dewarp());
        QVERIFY(s.circle());
        QCOMPARE(s.shownRatio(), 1.0);
    }

    void aFlatPictureFromAFisheyeIsNotDewarped()
    {
        // A fisheye camera whose picture is not square (an NVR's flattened copy, the go2rtc
        // kitchen_flat 1280 x 1600) has no circle to dewarp: it is shown as it is. Live and
        // Playback used to dewarp it anyway.
        PictureSurface s;
        showFlat(s);
        s.setFisheyeLens(true);
        s.setFisheyeMode(QStringLiteral("quad"));
        QVERIFY(!s.dewarp());
        QVERIFY(!s.circle());
        QCOMPARE(s.shownRatio(), 16.0 / 9);
        s.setPictureRatio(1280.0 / 1600);
        QVERIFY(!s.dewarp());
        QCOMPARE(s.shownRatio(), 1280.0 / 1600);
    }

    void anOrdinaryCameraIsNeverDewarped()
    {
        PictureSurface s;
        showCircle(s, QStringLiteral("quad"));
        s.setFisheyeLens(false);
        QVERIFY(!s.dewarp());
        QVERIFY(!s.circle());
    }

    void encoderRoundingStillCountsAsACircle()
    {
        PictureSurface s;
        showCircle(s, QStringLiteral("quad"));
        s.setPictureRatio(640.0 / 636);
        QVERIFY(s.dewarp());
        s.setPictureRatio(2560.0 / 2560);
        QVERIFY(s.dewarp());
        s.setPictureRatio(1.02);
        QVERIFY(!s.dewarp());
    }

    void beforeTheFirstPictureTheLensAndViewDecide()
    {
        // No picture seen (ratio 0): do not flash the flat picture before the first frame.
        PictureSurface s;
        s.setFisheyeLens(true);
        s.setFisheyeMode(QStringLiteral("quad"));
        QCOMPARE(s.pictureRatio(), 0.0);
        QVERIFY(s.dewarp());
        s.setFisheyeMode(QStringLiteral("fisheye"));
        QVERIFY(!s.dewarp());
    }

    void frigatesFlatCopyIsNeverDewarped()
    {
        PictureSurface s;
        showCircle(s, QStringLiteral("quad"));
        s.setFlatCopy(true);
        QVERIFY(!s.dewarp());
        QVERIFY(!s.circle());
        QCOMPARE(s.shownRatio(), 1.0);               // its own square, not the view's
    }

    void theAnswersSayWhenTheyChange()
    {
        PictureSurface s;
        QSignalSpy dewarp(&s, &PictureSurface::dewarpChanged);
        showCircle(s, QStringLiteral("fisheye"));
        const int before = dewarp.count();
        s.setFisheyeMode(QStringLiteral("quad"));
        QCOMPARE(dewarp.count(), before + 1);
        s.setFisheyeMode(QStringLiteral("panorama"));   // still a dewarp, but a new shape
        QCOMPARE(dewarp.count(), before + 2);
        QSignalSpy ratio(&s, &PictureSurface::ratioChanged);
        s.setFisheyeMode(QStringLiteral("fisheye"));
        s.setContentRect(QRectF(250, 0, 400, 400));
        QCOMPARE(ratio.count(), 0);                     // 1.0 again: no change
    }

    void aDewarpDropsTheDigitalZoom()
    {
        PictureSurface s;
        showCircle(s, QStringLiteral("fisheye"));
        s.setZoom(3);
        s.panBy(50, 50);
        s.setFisheyeMode(QStringLiteral("ptz"));        // a dewarped view zooms itself
        QCOMPARE(s.zoom(), 1.0);
        QCOMPARE(s.panX(), 0.0);
        QCOMPARE(s.panY(), 0.0);
    }

    // ---- aim ------------------------------------------------------------------------------
    void doubleClickPutsATurnedViewBack()
    {
        QSettings().remove(QStringLiteral("fisheyeAim"));
        FisheyeAims store;
        PictureSurface s;
        showCircle(s, QStringLiteral("quad"));
        s.setCameraKey(QStringLiteral("5:0"));
        s.setAims(&store);
        QVERIFY(!s.aimed());
        QVERIFY(!s.resetAim());                          // nothing turned: the pane maximises
        store.setAim(QStringLiteral("5:0"), QStringLiteral("quad"), 2, fisheye::Aim{1, 0.7, 1.5, 0});
        QVERIFY(s.aimed());
        QVERIFY(s.resetAim());
        QVERIFY(!store.aimed(QStringLiteral("5:0"), QStringLiteral("quad")));
        QVERIFY(!s.resetAim());                          // second double-click maximises
        QSettings().remove(QStringLiteral("fisheyeAim"));
    }

    void aimIsOnlyOfADewarpedView()
    {
        QSettings().remove(QStringLiteral("fisheyeAim"));
        FisheyeAims store;
        store.setAim(QStringLiteral("5:0"), QStringLiteral("quad"), 2, fisheye::Aim{1, 0.7, 1.5, 0});
        PictureSurface s;
        showFlat(s);                                     // not a circle: not dewarped
        s.setFisheyeLens(true);
        s.setFisheyeMode(QStringLiteral("quad"));
        s.setCameraKey(QStringLiteral("5:0"));
        s.setAims(&store);
        QVERIFY(!s.aimed());
        QVERIFY(!s.resetAim());
        QVERIFY(store.aimed(QStringLiteral("5:0"), QStringLiteral("quad")));
        QSettings().remove(QStringLiteral("fisheyeAim"));
    }

    void noStoreMeansNothingToReset()
    {
        PictureSurface s;
        showCircle(s, QStringLiteral("quad"));
        QVERIFY(!s.aimed());
        QVERIFY(!s.resetAim());
    }
};

QTEST_MAIN(TestPictureSurface)
#include "test_picturesurface.moc"
