#include "FrigateLive.h"

#include "device/FrigateApi.h"
#include "protocol/WebSocketFrames.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSslSocket>
#include <QThread>

#include <atomic>
#include <functional>
#include <mutex>

namespace rl {

namespace {
constexpr int kRetryMs = 5'000;
constexpr int kMaxRetryMs = 60'000;
constexpr int kTimeoutMs = 8'000;
constexpr qint64 kPingMs = 30'000;
constexpr qint64 kSilentMs = 75'000; // nothing back for this long (pings included): reconnect
constexpr int kReadWaitMs = 300;
std::atomic<int> g_workers{0};
} // namespace

struct FrigateLive::Shared {
    std::atomic<bool> stop{false};
    std::mutex lock;
    FrigateLive *owner = nullptr;

    void post(std::function<void(FrigateLive *)> f)
    {
        std::lock_guard<std::mutex> g(lock);
        if (FrigateLive *o = owner)
            QMetaObject::invokeMethod(o, [o, f] { f(o); }, Qt::QueuedConnection);
    }
    bool sleepFor(int ms)
    {
        QDeadlineTimer until(ms);
        while (!until.hasExpired()) {
            if (stop)
                return false;
            QThread::msleep(quint64(qMin<qint64>(200, until.remainingTime())));
        }
        return !stop;
    }
};

void FrigateLive::waitForWorkers(int ms)
{
    QDeadlineTimer until(ms);
    while (g_workers > 0 && !until.hasExpired())
        QThread::msleep(20);
}

FrigateLive::FrigateLive(const QString &host, int port, QObject *parent)
    : QObject(parent), m_host(host), m_port(port), m_shared(std::make_shared<Shared>())
{
    m_shared->owner = this;
    ++g_workers;
    // A QThread (not std::thread): signing in uses QNetworkAccessManager, which needs one.
    QThread *t = QThread::create(&FrigateLive::run, m_shared, host, port);
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

FrigateLive::~FrigateLive()
{
    std::lock_guard<std::mutex> g(m_shared->lock);
    m_shared->owner = nullptr;
    m_shared->stop = true;
}

void FrigateLive::run(std::shared_ptr<Shared> shared, QString host, int port)
{
    struct Done { ~Done() { --g_workers; } } done;
    auto setConnected = [&shared](bool on) {
        shared->post([on](FrigateLive *o) { emit o->connectedChanged(on); });
    };
    const bool tls = port == 8971; // as FrigateApi: 8971 is the authenticated HTTPS port
    int wait = kRetryMs;
    while (!shared->stop) {
        const QByteArray cookie = frigate::sessionCookie(host, port, kTimeoutMs);
        QSslSocket sock;
        bool up = false;
        if (tls) {
            sock.setPeerVerifyMode(QSslSocket::VerifyNone); // Frigate's self-signed certificate
            sock.connectToHostEncrypted(host, quint16(port));
            up = sock.waitForEncrypted(kTimeoutMs);
        } else {
            sock.connectToHost(host, quint16(port));
            up = sock.waitForConnected(kTimeoutMs);
        }

        QByteArray buf;
        if (up) {
            QByteArray nonce(16, Qt::Uninitialized);
            for (char &c : nonce)
                c = char(QRandomGenerator::global()->bounded(256));
            const QByteArray key = nonce.toBase64();
            QByteArray req = "GET /ws HTTP/1.1\r\nHost: " + host.toUtf8() + ':' + QByteArray::number(port)
                + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key
                + "\r\nSec-WebSocket-Version: 13\r\n";
            if (!cookie.isEmpty())
                req += "Cookie: " + cookie + "\r\n";
            req += "\r\n";
            sock.write(req);
            sock.flush();
            QDeadlineTimer until(kTimeoutMs);
            while (!buf.contains("\r\n\r\n") && !until.hasExpired() && !shared->stop)
                if (sock.waitForReadyRead(kReadWaitMs))
                    buf += sock.readAll();
            const int end = buf.indexOf("\r\n\r\n");
            const QByteArray head = end >= 0 ? buf.left(end).toLower() : QByteArray();
            if (head.startsWith("http/1.1 401"))
                frigate::forgetSession(host, port); // stale cookie: sign in afresh next time
            up = head.startsWith("http/1.1 101")
                 && head.contains("sec-websocket-accept: " + ws::acceptFor(key).toLower());
            buf.remove(0, end >= 0 ? end + 4 : buf.size());
        }

        if (up) {
            setConnected(true);
            wait = kRetryMs;
            QElapsedTimer sincePing, sinceHeard;
            sincePing.start();
            sinceHeard.start();
            QByteArray message; // text frames joined across continuations
            bool closing = false;
            while (!shared->stop && !closing && sock.state() == QAbstractSocket::ConnectedState) {
                if (sincePing.elapsed() >= kPingMs) {
                    sock.write(ws::clientFrame(ws::Ping, {}, QRandomGenerator::global()->generate()));
                    sincePing.restart();
                }
                if (sinceHeard.elapsed() >= kSilentMs)
                    break;
                if (sock.bytesAvailable() > 0 || sock.waitForReadyRead(kReadWaitMs))
                    buf += sock.readAll();
                ws::Frame f;
                while (!closing && ws::takeFrame(buf, f)) {
                    sinceHeard.restart();
                    switch (f.opcode) {
                    case ws::Text:
                    case ws::Continuation:
                        message += f.payload;
                        if (f.fin) {
                            frigate::Event e;
                            if (frigate::parseLiveEvent(message, &e))
                                shared->post([e](FrigateLive *o) { emit o->liveEvent(e.camera, e.label, e.start); });
                            message.clear();
                        }
                        break;
                    case ws::Ping:
                        sock.write(ws::clientFrame(ws::Pong, f.payload, QRandomGenerator::global()->generate()));
                        break;
                    case ws::Close:
                        closing = true;
                        break;
                    default:
                        break;
                    }
                }
            }
            setConnected(false);
        }
        sock.abort();
        if (!shared->sleepFor(wait))
            return;
        wait = qMin(wait * 2, kMaxRetryMs);
    }
}

} // namespace rl
