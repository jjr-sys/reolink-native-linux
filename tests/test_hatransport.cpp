// NetworkHomeAssistantTransport gives up on a request Home Assistant never answers
// (bosch-ptz-port ticket 02): a lost request must not leave a Bosch control waiting forever.
#include "device/HomeAssistantTransport.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace rl;

class TestHaTransport : public QObject
{
    Q_OBJECT

private slots:
    void aRequestNobodyAnswersEndsWithItsOwnError()
    {
        QTcpServer silent;   // accepts and reads, never replies
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        QList<QTcpSocket *> held;
        connect(&silent, &QTcpServer::newConnection, this, [&] {
            while (QTcpSocket *s = silent.nextPendingConnection())
                held << s;
        });

        NetworkHomeAssistantTransport transport(nullptr, 300);
        int calls = 0, status = -1;
        QString error;
        transport.send(QUrl(QStringLiteral("http://127.0.0.1:%1/api/services/x/y")
                                .arg(silent.serverPort())),
                       QStringLiteral("tok"), "{}",
                       [&](int s, const QByteArray &, const QString &e) {
            ++calls;
            status = s;
            error = e;
        });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);
        QCOMPARE(status, 0);
        QCOMPARE(error, QStringLiteral("No answer from Home Assistant in 1 s"));
        QTest::qWait(400);
        QCOMPARE(calls, 1);   // exactly once
        qDeleteAll(held);
    }

    void theDefaultLimitIsFortySeconds()
    {
        QCOMPARE(NetworkHomeAssistantTransport::kDefaultTimeoutMs, 40000);
    }
};

QTEST_MAIN(TestHaTransport)
#include "test_hatransport.moc"
