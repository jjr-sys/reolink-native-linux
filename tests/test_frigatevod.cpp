// Frigate Playback over the authenticated port: the player must sign in.
//
// Frigate's VOD playlist (/vod/<camera>/start/../end/../index.m3u8) on port 8971 needs the
// session cookie ("frigate_token=…") on the playlist AND on every segment it lists. The
// player used to open the bare URL, so every Frigate Playback answered 401 ("Sign-in
// refused"). This drives the real path: a registered Frigate login, the media auth hook,
// and a StreamPlayer opening an HLS playlist from a local server.
#include "device/FrigateApi.h"
#include "media/StreamPlayer.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace rl;

namespace {

// A tiny Frigate stand-in: /api/login hands out a cookie, the playlist and segment
// answer 401 unless the request carries a cookie the server accepts.
class FakeFrigate : public QObject
{
public:
    QTcpServer server;
    int logins = 0;
    QByteArray issue = "abc";   // the cookie the next login hands out
    QByteArray accept = "abc";  // the cookie the media endpoints accept ("" = none)
    QList<QByteArray> playlistCookies;
    QList<QByteArray> segmentCookies;

    FakeFrigate()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buf = new QByteArray;
                connect(s, &QTcpSocket::readyRead, this, [this, s, buf] {
                    *buf += s->readAll();
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    handle(s, buf->left(end));
                    buf->clear();
                });
                connect(s, &QTcpSocket::disconnected, s, [s, buf] {
                    delete buf;
                    s->deleteLater();
                });
            }
        });
    }
    int port() const { return server.serverPort(); }

    static QByteArray cookieOf(const QByteArray &head)
    {
        for (const QByteArray &line : head.split('\n'))
            if (line.toLower().startsWith("cookie:"))
                return line.mid(7).trimmed();
        return {};
    }

    void reply(QTcpSocket *s, const QByteArray &status, const QByteArray &type,
               const QByteArray &body, const QByteArray &extra = {})
    {
        s->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\n" + extra + "Connection: close\r\n\r\n"
                 + body);
        s->disconnectFromHost();
    }

    void handle(QTcpSocket *s, const QByteArray &head)
    {
        const QByteArray path = head.split(' ').value(1);
        const QByteArray cookie = cookieOf(head);
        const bool ok = !accept.isEmpty() && cookie == "frigate_token=" + accept;
        if (path == "/api/login") {
            ++logins;
            // Frigate reads a JSON body; the content does not matter here.
            reply(s, "200 OK", "application/json", "{}",
                  "Set-Cookie: frigate_token=" + issue + "; Path=/; HttpOnly\r\n");
        } else if (path.endsWith("index.m3u8")) {
            playlistCookies << cookie;
            if (!ok)
                return reply(s, "401 Unauthorized", "text/plain", "no");
            reply(s, "200 OK", "application/vnd.apple.mpegurl",
                  "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                  "#EXTINF:4.0,\nseg0.ts\n#EXT-X-ENDLIST\n");
        } else if (path.endsWith("seg0.ts")) {
            segmentCookies << cookie;
            if (!ok)
                return reply(s, "401 Unauthorized", "text/plain", "no");
            reply(s, "200 OK", "video/mp2t", QByteArray(188, '\0'));
        } else {
            reply(s, "404 Not Found", "text/plain", "");
        }
    }
};

QString vodUrl(int port)
{
    return frigate::vodUrl(QStringLiteral("127.0.0.1"), port, QStringLiteral("kitchen"), 1000, 2000);
}

} // namespace

class TestFrigateVod : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { frigate::installMediaAuth(); }
    // Each test registers a login for its own server's port; drop it so a later server that
    // happens to get the same port starts with no login.
    void cleanup()
    {
        if (m_port)
            frigate::setCredentials(QStringLiteral("127.0.0.1"), m_port, {}, {});
        m_port = 0;
    }

    // The playlist and the segment it lists both carry the Frigate session cookie.
    void playlistAndSegmentsCarryTheSessionCookie()
    {
        FakeFrigate f;
        QVERIFY(f.server.isListening());
        m_port = f.port();
        frigate::setCredentials(QStringLiteral("127.0.0.1"), f.port(), QStringLiteral("u"),
                                QStringLiteral("p"));

        StreamPlayer player;
        player.setSource(vodUrl(f.port()));
        player.start();

        QTRY_VERIFY_WITH_TIMEOUT(!f.segmentCookies.isEmpty(), 15000);
        player.stop();
        QCOMPARE(f.logins, 1);
        QCOMPARE(f.playlistCookies.first(), QByteArray("frigate_token=abc"));
        QCOMPARE(f.segmentCookies.first(), QByteArray("frigate_token=abc"));
    }

    // A refused cookie is forgotten: the retry signs in afresh and plays.
    void refusedCookieSignsInAgain()
    {
        FakeFrigate f;
        f.issue = "old";
        f.accept = "new";
        m_port = f.port();
        frigate::setCredentials(QStringLiteral("127.0.0.1"), f.port(), QStringLiteral("u"),
                                QStringLiteral("p"));

        StreamPlayer player;
        player.setRetryOnError(true);
        player.setSource(vodUrl(f.port()));
        player.start();

        QTRY_VERIFY_WITH_TIMEOUT(!f.playlistCookies.isEmpty(), 15000);
        f.issue = "new"; // the server rotated its secret: only a fresh sign-in works
        QTRY_VERIFY_WITH_TIMEOUT(!f.segmentCookies.isEmpty(), 15000);
        player.stop();
        QCOMPARE(f.logins, 2);
        QCOMPARE(f.segmentCookies.first(), QByteArray("frigate_token=new"));
    }

    // A cached session is reused: a second open does not sign in again.
    void cachedSessionIsReused()
    {
        FakeFrigate f;
        m_port = f.port();
        frigate::setCredentials(QStringLiteral("127.0.0.1"), f.port(), QStringLiteral("u"),
                                QStringLiteral("p"));
        for (int i = 0; i < 2; ++i) {
            StreamPlayer player;
            player.setSource(vodUrl(f.port()));
            player.start();
            QTRY_VERIFY_WITH_TIMEOUT(f.segmentCookies.size() > i, 15000);
            player.stop();
        }
        QCOMPARE(f.logins, 1);
        QCOMPARE(f.segmentCookies.last(), QByteArray("frigate_token=abc"));
    }

    // A host with no Frigate login (a Reolink NVR, port 5000 Frigate) sends no cookie and
    // never tries to sign in.
    void hostWithoutLoginSendsNoCookie()
    {
        FakeFrigate f;
        f.accept.clear(); // the media endpoints refuse everything; we only watch the request
        StreamPlayer player;
        player.setSource(vodUrl(f.port()));
        player.start();

        QTRY_VERIFY_WITH_TIMEOUT(!f.playlistCookies.isEmpty(), 15000);
        player.stop();
        QCOMPARE(f.logins, 0);
        QVERIFY(f.playlistCookies.first().isEmpty());
    }

private:
    int m_port = 0;
};

QTEST_MAIN(TestFrigateVod)
#include "test_frigatevod.moc"
