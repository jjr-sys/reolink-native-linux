#include "ui/FrameShape.h"

#include <QtTest>

#include <cmath>
#include <limits>

using namespace rl;

// Frames that take their picture's shape (wayfinder/fisheye-and-framing step 06): an Events
// feed card covers about the area of today's 16:9 card at the feed's width, never wider than
// the feed; the clip player shows the largest box of the picture's shape that fits. No black
// bars, no cropping.
class TestFrameShape : public QObject
{
    Q_OBJECT

private slots:
    void aWidePictureKeepsTodaysCard()
    {
        QCOMPARE(frameshape::cardSize(560, 16.0 / 9), QSize(560, 315));
    }

    void aPortraitPictureKeepsTheAreaAndIsCentredNarrower()
    {
        // The Woorabinda Doorbell, 640x853.
        const QSize s = frameshape::cardSize(560, 640.0 / 853);
        QCOMPARE(s, QSize(364, 485));
        QVERIFY(std::abs(s.width() * s.height() - 560 * 315) < 560 * 315 / 100);
    }

    // One shapes table for every page (wayfinder/playback-controls ticket 13).
    void eachFisheyeViewHasItsShape()
    {
        QCOMPARE(frameshape::viewRatio(QStringLiteral("quad")), 16.0 / 9);
        QCOMPARE(frameshape::viewRatio(QStringLiteral("fisheye")), 1.0);
        QCOMPARE(frameshape::viewRatio(QStringLiteral("ptz")), 16.0 / 9);
        QCOMPARE(frameshape::viewRatio(QStringLiteral("panorama")), 3.3);
        QCOMPARE(frameshape::viewRatio(QStringLiteral("dual")), 2.1 / 2);
        QCOMPARE(frameshape::viewRatio(QStringLiteral("fisheye_ptz")), 7.0 / 3);
        QCOMPARE(frameshape::viewRatio(QStringLiteral("nonsense")), 16.0 / 9);
    }

    void aSquareFisheyeCircle()
    {
        QCOMPARE(frameshape::cardSize(560, 1.0), QSize(420, 420));
    }

    void aVeryWidePictureIsCappedAtTheFeedWidth()
    {
        // The Main Gate, 640x180: the same area would be 752 wide.
        QCOMPARE(frameshape::cardSize(560, 640.0 / 180), QSize(560, 158));
    }

    void theCardFollowsTheFeedWidth()
    {
        QCOMPARE(frameshape::cardSize(400, 16.0 / 9), QSize(400, 225));
        QCOMPARE(frameshape::cardSize(400, 1.0), QSize(300, 300));
    }

    void aMissingRatioMeansSixteenByNine()
    {
        QCOMPARE(frameshape::cardSize(560, 0), QSize(560, 315));
        QCOMPARE(frameshape::cardSize(560, -2), QSize(560, 315));
        QCOMPARE(frameshape::cardSize(560, std::numeric_limits<double>::quiet_NaN()), QSize(560, 315));
        QCOMPARE(frameshape::cardSize(560, std::numeric_limits<double>::infinity()), QSize(560, 315));
    }

    void noWidthNoCard()
    {
        QCOMPARE(frameshape::cardSize(0, 1.0), QSize(0, 0));
    }

    void fitInIsWidthLimitedForAWidePicture()
    {
        QCOMPARE(frameshape::fitIn(1000, 800, 16.0 / 9), QSize(1000, 563));
    }

    void fitInIsHeightLimitedForATallPicture()
    {
        QCOMPARE(frameshape::fitIn(1000, 800, 0.75), QSize(600, 800));
        QCOMPARE(frameshape::fitIn(1000, 800, 1.0), QSize(800, 800));
    }

    void fitInWithNoRoomOrNoRatio()
    {
        QCOMPARE(frameshape::fitIn(0, 800, 1.0), QSize(0, 0));
        QCOMPARE(frameshape::fitIn(1000, -5, 1.0), QSize(0, 0));
        QCOMPARE(frameshape::fitIn(1600, 900, 0), QSize(1600, 900)); // 16:9 when unknown
    }

    // A camera's remembered shape (step 07, shared by Live and Playback): a new measurement
    // is saved only when it moves by at least 0.5 %, so decoder jitter doesn't rewrite it.
    void aFirstMeasurementIsAChange()
    {
        QVERIFY(frameshape::shapeChanged(0, 1.0));
        QVERIFY(frameshape::shapeChanged(-1, 1.0));
    }

    void jitterIsNotAChange()
    {
        QVERIFY(!frameshape::shapeChanged(16.0 / 9, 16.0 / 9 * 1.004));
        QVERIFY(!frameshape::shapeChanged(1.0, 0.996));
        QVERIFY(frameshape::shapeChanged(16.0 / 9, 1.0));
        QVERIFY(frameshape::shapeChanged(1.0, 1.005));
    }

    void noMeasurementIsNeverAChange()
    {
        QVERIFY(!frameshape::shapeChanged(1.0, 0));
        QVERIFY(!frameshape::shapeChanged(1.0, -3));
        QVERIFY(!frameshape::shapeChanged(1.0, std::numeric_limits<double>::quiet_NaN()));
    }

    // The shape a pane is drawn at: the remembered decoded shape (16:9 when never seen),
    // turned by a quarter rotation.
    void drawnShapeFollowsTheRememberedOne()
    {
        QCOMPARE(frameshape::drawnRatio(1.0, 0), 1.0);
        QCOMPARE(frameshape::drawnRatio(0.75, 0), 0.75);
        QCOMPARE(frameshape::drawnRatio(0, 0), 16.0 / 9);
    }

    void aQuarterRotationTurnsTheShape()
    {
        QCOMPARE(frameshape::drawnRatio(16.0 / 9, 90), 9.0 / 16);
        QCOMPARE(frameshape::drawnRatio(16.0 / 9, 270), 9.0 / 16);
        QCOMPARE(frameshape::drawnRatio(16.0 / 9, 180), 16.0 / 9);
        QCOMPARE(frameshape::drawnRatio(0, 90), 9.0 / 16);
    }

    void theQmlFacadeGivesTheSameAnswers()
    {
        FrameShape f;
        QCOMPARE(f.cardSize(560, 0.75), QSizeF(frameshape::cardSize(560, 0.75)));
        QCOMPARE(f.fitIn(1000, 800, 0.75), QSizeF(frameshape::fitIn(1000, 800, 0.75)));
        QCOMPARE(f.shapeChanged(1.0, 1.2), true);
        QCOMPARE(f.drawnRatio(0.75, 90), 1 / 0.75);
    }
};

QTEST_GUILESS_MAIN(TestFrameShape)
#include "test_frameshape.moc"
