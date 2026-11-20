#include "device/DetectionNotice.h"

#include <QtTest>

using rl::device::DetectionNotice;

// The desktop notice a detection raises: the camera over what it saw.
class TestDetectionNotice : public QObject
{
    Q_OBJECT

    static QString body(const char *type) { return DetectionNotice::of(type, "Front Door").body; }

private slots:
    void person() { QCOMPARE(body("person"), QStringLiteral("Person detected")); }
    void vehicle() { QCOMPARE(body("vehicle"), QStringLiteral("Vehicle detected")); }
    void pet() { QCOMPARE(body("pet"), QStringLiteral("Pet detected")); }
    void dogCatIsAPet() { QCOMPARE(body("dog_cat"), QStringLiteral("Pet detected")); }
    void visitor() { QCOMPARE(body("visitor"), QStringLiteral("Visitor at the door")); }
    void motion() { QCOMPARE(body("motion"), QStringLiteral("Motion detected")); }
    void anUnknownTypeReadsAsMotion()
    {
        QCOMPARE(body("package"), QStringLiteral("Motion detected"));
    }

    void theTitleIsTheCamera()
    {
        QCOMPARE(DetectionNotice::of("person", "Front Door").title, QStringLiteral("Front Door"));
    }
    void aCameraWithoutANameIsCamera()
    {
        QCOMPARE(DetectionNotice::of("person", QString()).title, QStringLiteral("Camera"));
    }

    // The strings keep EventManager's translation context, where they were first marked.
    void translatedInEventManagersContext()
    {
        struct Fake : QTranslator {
            QString translate(const char *context, const char *source, const char *,
                              int) const override
            {
                if (QByteArrayView(context) == "rl::EventManager")
                    return QStringLiteral("[%1]").arg(QLatin1String(source));
                return {};
            }
            bool isEmpty() const override { return false; }
        } fake;
        QCoreApplication::installTranslator(&fake);
        const DetectionNotice n = DetectionNotice::of("visitor", QString());
        QCoreApplication::removeTranslator(&fake);
        QCOMPARE(n.body, QStringLiteral("[Visitor at the door]"));
        QCOMPARE(n.title, QStringLiteral("[Camera]"));
    }
};

QTEST_GUILESS_MAIN(TestDetectionNotice)
#include "test_detectionnotice.moc"
