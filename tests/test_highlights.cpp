#include "device/HighlightPolicy.h"

#include <QtTest>

using namespace rl::highlight;

namespace {
constexpr qint64 T0 = 1'000'000; // fake clock, ms
constexpr qint64 s(int n) { return n * 1000LL; }
const QString A = QStringLiteral("1:3");
const QString B = QStringLiteral("1:4");
const QString Chirp = QStringLiteral("bird-chirp");
const QString Horn = QStringLiteral("horn");

// Defaults, with a chirp for people and a horn for vehicles.
HighlightPolicy withSounds()
{
    Rules r = defaultRules();
    r[Kind::Person].sound = Chirp;
    r[Kind::Vehicle].sound = Horn;
    return HighlightPolicy(r);
}
} // namespace

class TestHighlights : public QObject
{
    Q_OBJECT
private slots:
    void defaultsHighlightPeopleVehiclesPetsButNotMotionAndAreSilent()
    {
        HighlightPolicy p;
        QVERIFY(p.detect(A, Kind::Person, T0).isEmpty());
        QCOMPARE(p.look(A, T0).kind, Kind::Person);
        QCOMPARE(p.look(A, T0).colour, QStringLiteral("#f5c518"));
        QCOMPARE(p.look(A, T0).style, Style::FlashThenSteady);
        p.detect(B, Kind::Motion, T0);
        QCOMPARE(p.look(B, T0).kind, Kind::None);
    }

    void aHighlightLastsTheRulesSecondsAndFlashesForThree()
    {
        HighlightPolicy p;
        p.detect(A, Kind::Pet, T0);
        const Look l = p.look(A, T0 + s(1));
        QCOMPARE(l.until, T0 + s(15));
        QCOMPARE(l.flashUntil, T0 + kFlashMs);
        QCOMPARE(p.look(A, T0 + s(15)).kind, Kind::None);
        QVERIFY(p.expire(T0 + s(15)));
        QVERIFY(p.highlighted(T0 + s(15)).isEmpty());
        QVERIFY(!p.expire(T0 + s(16)));
    }

    void stylesSetTheFlashTime()
    {
        Rules r = defaultRules();
        r[Kind::Person].style = Style::Steady;
        r[Kind::Vehicle].style = Style::Flashing;
        HighlightPolicy p(r);
        p.detect(A, Kind::Person, T0);
        p.detect(B, Kind::Vehicle, T0);
        QCOMPARE(p.look(A, T0).flashUntil, 0);
        QCOMPARE(p.look(B, T0).flashUntil, T0 + s(15));
    }

    // A repeat extends to a full duration from now; the flash does not restart.
    void aRepeatExtendsTheHighlightButNotTheFlash()
    {
        HighlightPolicy p;
        p.detect(A, Kind::Person, T0);
        p.detect(A, Kind::Person, T0 + s(10));
        const Look l = p.look(A, T0 + s(20));
        QCOMPARE(l.kind, Kind::Person);
        QCOMPARE(l.since, T0);
        QCOMPARE(l.until, T0 + s(25));
        QCOMPARE(l.last, T0 + s(10));
        QCOMPARE(l.flashUntil, T0 + kFlashMs);
    }

    void aDetectionAfterTheEndStartsAFreshHighlight()
    {
        HighlightPolicy p;
        p.detect(A, Kind::Person, T0);
        p.detect(A, Kind::Person, T0 + s(40));
        QCOMPARE(p.look(A, T0 + s(40)).since, T0 + s(40));
        QCOMPARE(p.look(A, T0 + s(40)).flashUntil, T0 + s(40) + kFlashMs);
    }

    void theHigherRankSetsTheLookAndTheLowerShowsWhenItEnds()
    {
        Rules r = defaultRules();
        r[Kind::Person].seconds = 5;
        HighlightPolicy p(r);
        p.detect(A, Kind::Vehicle, T0);
        p.detect(A, Kind::Person, T0 + s(1));
        QCOMPARE(p.look(A, T0 + s(2)).kind, Kind::Person);
        p.detect(A, Kind::Pet, T0 + s(3));
        QCOMPARE(p.look(A, T0 + s(4)).kind, Kind::Person);
        QCOMPARE(p.look(A, T0 + s(7)).kind, Kind::Vehicle);
        QCOMPARE(p.look(A, T0 + s(7)).colour, QStringLiteral("#3d8bfd"));
    }

    void chosenCamerasOnly()
    {
        Rules r = defaultRules();
        r[Kind::Person].allCameras = false;
        r[Kind::Person].cameras = {B};
        HighlightPolicy p(r);
        p.detect(A, Kind::Person, T0);
        p.detect(B, Kind::Person, T0);
        QCOMPARE(p.look(A, T0).kind, Kind::None);
        QCOMPARE(p.look(B, T0).kind, Kind::Person);
    }

    void overridesChangeColourSoundOrSwitchTheTypeOff()
    {
        Rules r = defaultRules();
        r[Kind::Person].sound = Chirp;
        r[Kind::Person].overrides[A].colour = QStringLiteral("#ff0000");
        r[Kind::Person].overrides[A].sound = QStringLiteral("none");
        r[Kind::Vehicle].overrides[B].off = true;
        HighlightPolicy p(r);
        QVERIFY(p.detect(A, Kind::Person, T0).isEmpty());
        QCOMPARE(p.look(A, T0).colour, QStringLiteral("#ff0000"));
        p.detect(B, Kind::Vehicle, T0);
        QCOMPARE(p.look(B, T0).kind, Kind::None);
        QCOMPARE(p.detect(B, Kind::Person, T0 + s(5)), Chirp);
    }

    void aSoundReplaysOnlyAfterItsCooldown()
    {
        HighlightPolicy p = withSounds();
        QCOMPARE(p.detect(A, Kind::Person, T0), Chirp);
        QVERIFY(p.detect(A, Kind::Person, T0 + s(30)).isEmpty());
        QVERIFY(p.detect(A, Kind::Person, T0 + s(59)).isEmpty());
        QCOMPARE(p.detect(A, Kind::Person, T0 + s(60)), Chirp);
    }

    // The cooldown is per camera × type: another camera or type has its own.
    void cooldownIsPerCameraAndType()
    {
        HighlightPolicy p = withSounds();
        QCOMPARE(p.detect(A, Kind::Person, T0), Chirp);
        QCOMPARE(p.detect(B, Kind::Person, T0 + s(3)), Chirp);
        QCOMPARE(p.detect(A, Kind::Vehicle, T0 + s(6)), Horn);
    }

    void soundsCloseTogetherCollapseToTheFirst()
    {
        HighlightPolicy p = withSounds();
        QCOMPARE(p.detect(A, Kind::Person, T0), Chirp);
        QVERIFY(p.detect(B, Kind::Vehicle, T0 + 1500).isEmpty());
        // The collapsed one did not start its cooldown, so it can sound once clear.
        QCOMPARE(p.detect(B, Kind::Vehicle, T0 + s(3)), Horn);
        // Its highlight was not lost either.
        QCOMPARE(p.look(B, T0 + s(3)).kind, Kind::Vehicle);
    }

    void resetForgetsEverything()
    {
        HighlightPolicy p = withSounds();
        p.detect(A, Kind::Person, T0);
        p.reset();
        QCOMPARE(p.look(A, T0).kind, Kind::None);
        QCOMPARE(p.detect(A, Kind::Person, T0 + s(1)), Chirp);
    }

    void namesRoundTrip()
    {
        for (Kind k : {Kind::Motion, Kind::Pet, Kind::Vehicle, Kind::Person})
            QCOMPARE(kindOf(nameOf(k)), k);
        QCOMPARE(kindOf(QStringLiteral("dog_cat")), Kind::Pet);
        QCOMPARE(kindOf(QStringLiteral("visitor")), Kind::None);
        for (Style st : {Style::Steady, Style::Flashing, Style::FlashThenSteady})
            QCOMPARE(styleOf(nameOf(st)), st);
    }
};

QTEST_GUILESS_MAIN(TestHighlights)
#include "test_highlights.moc"
