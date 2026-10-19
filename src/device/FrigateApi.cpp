#include "FrigateApi.h"

#include <QEventLoop>
#include <algorithm>
#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

namespace rl::frigate {

QString displayCameraName(const QString &name)
{
    if (name.contains(QLatin1Char(' ')))
        return name;
    QStringList words = name.split(QRegularExpression(QStringLiteral("[_-]+")), Qt::SkipEmptyParts);
    for (QString &w : words)
        w[0] = w[0].toUpper();
    return words.join(QLatin1Char(' '));
}

QStringList parseCameraNames(const Json &config)
{
    QStringList out;
    if (!config.is_object() || !config.contains("cameras") || !config["cameras"].is_object())
        return out;
    for (auto it = config["cameras"].begin(); it != config["cameras"].end(); ++it) {
        const Json &cam = it.value();
        if (cam.is_object() && cam.contains("enabled") && cam["enabled"].is_boolean()
            && !cam["enabled"].get<bool>())
            continue;
        out.append(QString::fromStdString(it.key()));
    }
    return out;
}

QVector<Event> parseEvents(const Json &events)
{
    QVector<Event> out;
    if (!events.is_array())
        return out;
    for (const Json &e : events) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_string() || !e.contains("camera")
            || !e["camera"].is_string() || !e.contains("start_time") || !e["start_time"].is_number())
            continue;
        Event ev;
        ev.id = QString::fromStdString(e["id"].get<std::string>());
        ev.camera = QString::fromStdString(e["camera"].get<std::string>());
        ev.label = e.contains("label") && e["label"].is_string()
                       ? QString::fromStdString(e["label"].get<std::string>())
                       : QString();
        ev.start = e["start_time"].get<double>();
        out.append(ev);
    }
    return out;
}

QString typeForLabel(const QString &label)
{
    if (label == QLatin1String("person"))
        return QStringLiteral("person");
    for (const char *v : {"car", "truck", "bus", "motorcycle", "bicycle"})
        if (label == QLatin1String(v))
            return QStringLiteral("vehicle");
    if (label == QLatin1String("dog") || label == QLatin1String("cat"))
        return QStringLiteral("pet");
    return {};
}

double newWatermark(double current, const QVector<Event> &events)
{
    double w = current;
    for (const Event &e : events)
        w = qMax(w, e.start);
    return w;
}

static QString base(const QString &host, int port)
{
    const QString scheme = port == 8971 ? QStringLiteral("https") : QStringLiteral("http");
    return QStringLiteral("%1://%2:%3").arg(scheme, host).arg(port);
}
static QString enc(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QVector<Recording> parseRecordings(const Json &recordings)
{
    QVector<Recording> out;
    if (!recordings.is_array())
        return out;
    for (const Json &e : recordings) {
        if (!e.is_object() || !e.contains("start_time") || !e["start_time"].is_number()
            || !e.contains("end_time") || !e["end_time"].is_number())
            continue;
        Recording r;
        r.start = e["start_time"].get<double>();
        r.end = e["end_time"].get<double>();
        if (r.end > r.start)
            out.append(r);
    }
    return out;
}

QVector<Recording> mergeRecordings(QVector<Recording> segments, double gapSecs)
{
    std::sort(segments.begin(), segments.end(),
              [](const Recording &a, const Recording &b) { return a.start < b.start; });
    QVector<Recording> out;
    for (const Recording &r : segments) {
        if (!out.isEmpty() && r.start - out.last().end <= gapSecs)
            out.last().end = qMax(out.last().end, r.end);
        else
            out.append(r);
    }
    return out;
}

QList<int> parseRecordingDays(const Json &summary, int year, int month)
{
    QList<int> days;
    if (!summary.is_array())
        return days;
    const QString prefix = QStringLiteral("%1-%2-").arg(year, 4, 10, QLatin1Char('0')).arg(month, 2, 10, QLatin1Char('0'));
    for (const Json &d : summary) {
        if (!d.is_object() || !d.contains("day") || !d["day"].is_string() || !d.contains("hours")
            || !d["hours"].is_array())
            continue;
        const QString day = QString::fromStdString(d["day"].get<std::string>());
        if (!day.startsWith(prefix))
            continue;
        double recorded = 0;
        for (const Json &h : d["hours"])
            if (h.is_object() && h.contains("duration") && h["duration"].is_number())
                recorded += h["duration"].get<double>();
        bool ok = false;
        const int n = day.mid(prefix.size()).toInt(&ok);
        if (ok && recorded > 0)
            days.append(n);
    }
    std::sort(days.begin(), days.end());
    return days;
}

QString recordingsUrl(const QString &host, int port, const QString &camera, qint64 after, qint64 before)
{
    return base(host, port) + QStringLiteral("/api/") + enc(camera)
           + QStringLiteral("/recordings?after=%1&before=%2").arg(after).arg(before);
}

QString summaryUrl(const QString &host, int port, const QString &camera, const QString &timeZone)
{
    return base(host, port) + QStringLiteral("/api/") + enc(camera)
           + QStringLiteral("/recordings/summary?timezone=") + enc(timeZone);
}

QString vodUrl(const QString &host, int port, const QString &camera, qint64 start, qint64 end)
{
    return base(host, port) + QStringLiteral("/vod/") + enc(camera)
           + QStringLiteral("/start/%1/end/%2/index.m3u8").arg(start).arg(end);
}

QString configUrl(const QString &host, int port) { return base(host, port) + QStringLiteral("/api/config"); }

QString eventsUrl(const QString &host, int port, double after, int limit)
{
    return base(host, port)
           + QStringLiteral("/api/events?after=%1&limit=%2").arg(QString::number(after, 'g', 15)).arg(limit);
}

QString liveUrl(const QString &host, int port, const QString &camera)
{
    Q_UNUSED(port);
    return QStringLiteral("rtsp://%1:8554/%2").arg(host, enc(camera));
}

QString clipUrl(const QString &host, int port, const QString &camera, qint64 timestamp)
{
    return base(host, port) + QStringLiteral("/api/") + enc(camera)
           + QStringLiteral("/start/%1/end/%2/clip.mp4")
                 .arg(timestamp - kClipPreSecs)
                 .arg(timestamp + kClipPostSecs);
}

QString latestFrameUrl(const QString &host, int port, const QString &camera)
{
    return base(host, port) + QStringLiteral("/api/") + enc(camera) + QStringLiteral("/latest.jpg");
}

QString snapshotUrl(const QString &host, int port, const QString &eventId)
{
    return base(host, port) + QStringLiteral("/api/events/") + enc(eventId) + QStringLiteral("/snapshot.jpg");
}

namespace {

struct AuthEntry {
    QString username;
    QString password;
    QByteArray cookie;
    QDateTime expiresAt;
};

QMutex &authMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, AuthEntry> &authEntries()
{
    static QHash<QString, AuthEntry> entries;
    return entries;
}

QString authKey(const QString &host, int port)
{
    return host.trimmed().toLower() + QLatin1Char(':') + QString::number(port);
}

QString authKey(const QUrl &url)
{
    return authKey(url.host(), url.port(url.scheme() == QLatin1String("https") ? 443 : 80));
}

void configureTls(QNetworkRequest &request)
{
    if (request.url().scheme() != QLatin1String("https"))
        return;

    // Frigate commonly uses its generated, self-signed certificate on port 8971.
    // The connection remains encrypted and is scoped by the host's private tailnet.
    QSslConfiguration ssl = request.sslConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(ssl);
}

QByteArray awaitReply(QNetworkReply *reply, int timeoutMs, QString *error,
                      int *status = nullptr, QByteArray *setCookie = nullptr)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status)
        *status = code;
    if (setCookie)
        *setCookie = reply->rawHeader("Set-Cookie");

    QByteArray body;
    if (!reply->isFinished()) {
        if (error)
            *error = QStringLiteral("timed out");
        reply->abort();
    } else {
        body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError && error) {
            if (code == 401 || code == 403)
                *error = QStringLiteral("Frigate sign-in failed");
            else if (code >= 400)
                *error = QStringLiteral("Frigate returned HTTP %1").arg(code);
            else
                *error = reply->errorString();
        }
    }
    delete reply;
    return body;
}

QByteArray cookieValue(const QByteArray &setCookie)
{
    const QList<QNetworkCookie> cookies = QNetworkCookie::parseCookies(setCookie);
    for (const QNetworkCookie &cookie : cookies)
        if (cookie.name() == QByteArrayLiteral("frigate_token"))
            return cookie.name() + QByteArrayLiteral("=") + cookie.value();
    return {};
}

QByteArray sessionCookie(const QUrl &target, QNetworkAccessManager &nam,
                         int timeoutMs, QString *error)
{
    QMutexLocker lock(&authMutex());
    auto it = authEntries().find(authKey(target));
    if (it == authEntries().end() || it->username.isEmpty() || it->password.isEmpty())
        return {};

    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!it->cookie.isEmpty() && it->expiresAt > now)
        return it->cookie;

    QUrl loginUrl = target;
    loginUrl.setPath(QStringLiteral("/api/login"));
    loginUrl.setQuery(QString());
    QNetworkRequest request(loginUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    configureTls(request);
    const QJsonObject credentials{
        {QStringLiteral("user"), it->username},
        {QStringLiteral("password"), it->password},
    };
    const QByteArray body = QJsonDocument(credentials).toJson(QJsonDocument::Compact);
    int status = 0;
    QByteArray setCookie;
    QString loginError;
    awaitReply(nam.post(request, body), timeoutMs, &loginError, &status, &setCookie);
    const QByteArray cookie = cookieValue(setCookie);
    if (status != 200 || cookie.isEmpty()) {
        if (error)
            *error = status == 401 || status == 403
                         ? QStringLiteral("Frigate sign-in failed")
                         : status >= 400 ? QStringLiteral("Frigate login returned HTTP %1").arg(status)
                                         : loginError.isEmpty() ? QStringLiteral("Frigate login returned no session")
                                                                : loginError;
        return {};
    }

    it->cookie = cookie;
    it->expiresAt = now.addSecs(6 * 60 * 60);
    return it->cookie;
}

void clearSession(const QUrl &url)
{
    QMutexLocker lock(&authMutex());
    auto it = authEntries().find(authKey(url));
    if (it != authEntries().end()) {
        it->cookie.clear();
        it->expiresAt = {};
    }
}

} // namespace

void setCredentials(const QString &host, int port, const QString &username,
                    const QString &password)
{
    QMutexLocker lock(&authMutex());
    const QString key = authKey(host, port);
    if (username.isEmpty() || password.isEmpty()) {
        authEntries().remove(key);
        return;
    }
    AuthEntry &entry = authEntries()[key];
    if (entry.username != username || entry.password != password) {
        entry.username = username;
        entry.password = password;
        entry.cookie.clear();
        entry.expiresAt = {};
    }
}

bool parseLiveEvent(const QByteArray &message, Event *out)
{
    const QJsonObject m = QJsonDocument::fromJson(message).object();
    if (m.value(QLatin1String("topic")).toString() != QLatin1String("events"))
        return false;
    const QJsonValue p = m.value(QLatin1String("payload"));
    const QJsonObject payload = p.isString() ? QJsonDocument::fromJson(p.toString().toUtf8()).object()
                                             : p.toObject();
    if (payload.value(QLatin1String("type")).toString() != QLatin1String("new"))
        return false;
    const QJsonObject after = payload.value(QLatin1String("after")).toObject();
    Event e;
    e.id = after.value(QLatin1String("id")).toString();
    e.camera = after.value(QLatin1String("camera")).toString();
    e.label = after.value(QLatin1String("label")).toString();
    e.start = after.value(QLatin1String("start_time")).toDouble();
    if (e.camera.isEmpty() || e.label.isEmpty() || e.start <= 0)
        return false;
    *out = e;
    return true;
}

QByteArray sessionCookie(const QString &host, int port, int timeoutMs, QString *error)
{
    QNetworkAccessManager nam;
    return sessionCookie(QUrl(base(host, port)), nam, timeoutMs, error);
}

void forgetSession(const QString &host, int port)
{
    clearSession(QUrl(base(host, port)));
}

QByteArray httpGet(const QString &url, int timeoutMs, QString *error)
{
    QNetworkAccessManager nam; // thread-local: safe on any worker thread
    if (error)
        error->clear();
    const QUrl target(url);
    const QByteArray cookie = sessionCookie(target, nam, timeoutMs, error);
    if (error && !error->isEmpty())
        return {};

    auto get = [&](const QByteArray &session) {
        QNetworkRequest request(target);
        if (!session.isEmpty())
            request.setRawHeader("Cookie", session);
        configureTls(request);
        int status = 0;
        QByteArray body = awaitReply(nam.get(request), timeoutMs, error, &status);
        return qMakePair(body, status);
    };

    auto result = get(cookie);
    if (result.second == 401 && !cookie.isEmpty()) {
        clearSession(target);
        if (error)
            error->clear();
        const QByteArray renewedCookie = sessionCookie(target, nam, timeoutMs, error);
        if (error && !error->isEmpty())
            return {};
        result = get(renewedCookie);
    }
    const QByteArray body = result.first;
    return body;
}

} // namespace rl::frigate
