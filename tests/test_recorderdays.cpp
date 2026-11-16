#include "device/RecorderDays.h"
#include "ScriptedTransports.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

using namespace rl;

// One camera's day of recordings, from either vendor, as one dated answer keyed by camera
// (wayfinder/architecture ticket 01).
class TestRecorderDays : public QObject
{
    Q_OBJECT

    static qint64 at(const QDate &d, int h, int m = 0, int s = 0)
    {
        return QDateTime(d, QTime(h, m, s)).toSecsSinceEpoch();
    }

    // A Frigate stand-in: answers each GET whose path starts with one of `answers`' keys
    // with that JSON; anything else is taken and never answered.
    struct FakeFrigate : QTcpServer
    {
        QHash<QString, QByteArray> answers;
        QStringList asked;
        FakeFrigate()
        {
            listen(QHostAddress::LocalHost);
            connect(this, &QTcpServer::newConnection, this, [this] {
                QTcpSocket *s = nextPendingConnection();
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    const QByteArray line = s->readLine();
                    const QString path = QString::fromLatin1(line.split(' ').value(1));
                    asked << path;
                    for (auto it = answers.cbegin(); it != answers.cend(); ++it)
                        if (path.startsWith(it.key())) {
                            s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                     "Content-Length: " + QByteArray::number(it.value().size())
                                     + "\r\nConnection: close\r\n\r\n" + it.value());
                            s->disconnectFromHost();
                            return;
                        }
                });
            });
        }
    };

private slots:
    // The timeline's segments: seconds into the local day, clamped to it, with the file's
    // own start kept for playing from it.
    void segmentsAreSecondsIntoTheDayClampedToIt()
    {
        const QDate d(2026, 11, 16);
        const QVector<DayFile> files = {
            {double(at(d.addDays(-1), 23, 50)), double(at(d, 0, 10))},   // from the day before
            {double(at(d, 9)), double(at(d, 9, 30))},
            {double(at(d, 23, 55)), double(at(d.addDays(1), 0, 5))},     // into the next day
        };
        const QVariantList segs = daySegments(files, d);
        QCOMPARE(segs.size(), 3);
        auto seg = [&](int i) { return segs.at(i).toMap(); };
        QCOMPARE(seg(0)["start"].toDouble(), 0.0);
        QCOMPARE(seg(0)["end"].toDouble(), 600.0);
        QCOMPARE(seg(1)["start"].toDouble(), 32400.0);
        QCOMPARE(seg(1)["end"].toDouble(), 34200.0);
        QCOMPARE(seg(1)["startEpoch"].toDouble(), double(at(d, 9)));
        QCOMPARE(seg(1)["type"].toString(), QStringLiteral("timer"));
        QCOMPARE(seg(2)["start"].toDouble(), 86100.0);
        QCOMPARE(seg(2)["end"].toDouble(), 86400.0);
    }

    // A Reolink recorder's day: one sub-stream Search for the camera's channel over the day;
    // its files and the month's recording days (as real RLN8-410 firmware answers).
    void aReolinkDayIsOneSearch()
    {
        auto http = std::make_shared<testing::ScriptedHttp>();
        const QByteArray reply = R"({"SearchResult":{"channel":3,
            "Status":[{"mon":7,"year":2026,"table":"0000011110000000000000000000000"}],
            "File":[{"size":"10485760","type":"sub",
             "StartTime":{"year":2026,"mon":7,"day":9,"hour":8,"min":30,"sec":0},
             "PlaybackTime":{"year":2026,"mon":7,"day":9,"hour":4,"min":59,"sec":58},
             "EndTime":{"year":2026,"mon":7,"day":9,"hour":8,"min":31,"sec":0}}]}})";
        http->values["Search"] = Json::parse(reply.constData(), reply.constData() + reply.size());
        const QDate d(2026, 7, 9);
        const DayAnswer a = reolinkDaySearch(http, 3)(d, nullptr);
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        QCOMPARE(http->sent, QStringList({QStringLiteral("Search")}));
        const Json &search = http->sentParams.first()["Search"];
        QCOMPARE(search.value("channel", -1), 3);
        QCOMPARE(QString::fromStdString(search.value("streamType", std::string{})), QStringLiteral("sub"));
        QCOMPARE(a.files.size(), 1);
        QCOMPARE(a.files.first().startEpoch, double(at(d, 8, 30)));
        QCOMPARE(a.files.first().endEpoch, double(at(d, 8, 31)));
        QCOMPARE(a.monthDays, QList<int>({6, 7, 8, 9}));

        http->transportOk = false;
        http->transportError = QStringLiteral("no route to host");
        QCOMPARE(reolinkDaySearch(http, 3)(d, nullptr).error, QStringLiteral("no route to host"));
    }

    // A Frigate camera's day: its recordings over the local day (merged where they touch) and
    // the month's days with footage from the summary.
    void aFrigateDayIsItsRecordingsAndSummary()
    {
        FakeFrigate frigate;
        const QDate d(2026, 9, 20);
        const qint64 nine = at(d, 9);
        frigate.answers["/api/drive/recordings?"] = QStringLiteral(
            R"([{"start_time":%1,"end_time":%2},{"start_time":%2.5,"end_time":%3}])")
            .arg(nine).arg(nine + 10).arg(nine + 20).toLatin1();
        frigate.answers["/api/drive/recordings/summary"] =
            R"([{"day":"2026-09-20","hours":[{"hour":"09","duration":20}]},
                {"day":"2026-09-05","hours":[{"hour":"10","duration":10}]}])";
        const DayAnswer a = frigateDaySearch(QStringLiteral("127.0.0.1"), frigate.serverPort(),
                                             QStringLiteral("drive"))(d, nullptr);
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        QCOMPARE(a.files.size(), 1);
        QCOMPARE(a.files.first().startEpoch, double(nine));
        QCOMPARE(a.files.first().endEpoch, double(nine + 20));
        QCOMPARE(a.monthDays, QList<int>({5, 20}));
        QVERIFY(frigate.asked.first().contains(QStringLiteral("after=%1").arg(d.startOfDay().toSecsSinceEpoch())));
    }

    // A Frigate search stops when cancelled (the module going), not after its 20 s timeout.
    void aFrigateSearchStopsWhenCancelled()
    {
        FakeFrigate frigate;   // never answers
        const quint16 port = frigate.serverPort();
        std::atomic<bool> cancel{false};
        DayAnswer a;
        QThread *worker = QThread::create([&] {
            a = frigateDaySearch(QStringLiteral("127.0.0.1"), port, QStringLiteral("drive"))(
                QDate(2026, 9, 20), &cancel);
        });
        worker->start();
        QTRY_COMPARE(frigate.asked.size(), 1);
        QElapsedTimer took;
        took.start();
        cancel = true;
        QVERIFY(worker->wait(2000));
        QVERIFY(took.elapsed() < 2000);
        QVERIFY(!a.error.isEmpty());
        delete worker;
    }

    // The answer names the camera and the day it is for, so a page showing another day (or a
    // camera whose row moved meanwhile) can tell it isn't its own.
    void theAnswerIsKeyedAndDated()
    {
        const QDate d(2026, 11, 16);
        RecorderDays days([&](const QString &key) -> DaySearch {
            if (key != QStringLiteral("1:6"))
                return {};   // not ready
            return [d](const QDate &asked, const std::atomic<bool> *) {
                DayAnswer a;
                if (asked != d) {
                    a.error = QStringLiteral("nothing that day");
                    return a;
                }
                a.files = {{double(at(d, 9)), double(at(d, 10))}};
                a.monthDays = {2, 16};
                return a;
            };
        });
        QSignalSpy found(&days, &RecorderDays::dayFound);
        QSignalSpy failed(&days, &RecorderDays::dayFailed);

        days.search(QStringLiteral("1:6"), 2026, 11, 16);
        QTRY_COMPARE(found.count(), 1);
        const QList<QVariant> f = found.first();
        QCOMPARE(f.at(0).toString(), QStringLiteral("1:6"));
        QCOMPARE(QList<int>({f.at(1).toInt(), f.at(2).toInt(), f.at(3).toInt()}), QList<int>({2026, 11, 16}));
        QCOMPARE(f.at(4).toList().size(), 1);
        QCOMPARE(f.at(4).toList().first().toMap()["start"].toDouble(), 32400.0);
        QCOMPARE(f.at(5).toList(), QVariantList({2, 16}));

        days.search(QStringLiteral("1:6"), 2026, 11, 15);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(), QStringLiteral("1:6"));
        QCOMPARE(failed.first().at(3).toInt(), 15);
        QCOMPARE(failed.first().at(4).toString(), QStringLiteral("nothing that day"));

        days.search(QStringLiteral("2:2"), 2026, 11, 16);   // its device isn't ready
        QTRY_COMPARE(failed.count(), 2);
        QCOMPARE(failed.last().at(0).toString(), QStringLiteral("2:2"));
        QVERIFY(!failed.last().at(4).toString().isEmpty());
        QCOMPARE(found.count(), 1);
    }

    // The module going (the app quitting) stops a search in flight and doesn't wait it out.
    void goingStopsASearchInFlight()
    {
        std::atomic<bool> started{false}, stopped{false};
        auto *days = new RecorderDays([&](const QString &) -> DaySearch {
            return [&](const QDate &, const std::atomic<bool> *cancel) {
                started = true;
                QElapsedTimer t;
                t.start();
                while (!(cancel && cancel->load()) && t.elapsed() < 20000)
                    QThread::msleep(10);
                stopped = cancel && cancel->load();
                return DayAnswer{};
            };
        });
        days->search(QStringLiteral("1:6"), 2026, 11, 16);
        QTRY_VERIFY(started.load());
        QElapsedTimer took;
        took.start();
        delete days;
        QVERIFY2(took.elapsed() < 2000, "going must not wait out the search");
        QVERIFY(stopped.load());
    }
};

QTEST_GUILESS_MAIN(TestRecorderDays)
#include "test_recorderdays.moc"
