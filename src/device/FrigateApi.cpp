#include "FrigateApi.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace rl::frigate {

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
    return QStringLiteral("http://%1:%2").arg(host).arg(port);
}
static QString enc(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QString configUrl(const QString &host, int port) { return base(host, port) + QStringLiteral("/api/config"); }

QString eventsUrl(const QString &host, int port, double after, int limit)
{
    return base(host, port)
           + QStringLiteral("/api/events?after=%1&limit=%2").arg(QString::number(after, 'g', 15)).arg(limit);
}

QString liveUrl(const QString &host, int port, const QString &camera)
{
    return base(host, port) + QStringLiteral("/api/go2rtc/api/stream.flv?src=") + enc(camera);
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

QByteArray httpGet(const QString &url, int timeoutMs, QString *error)
{
    QNetworkAccessManager nam; // thread-local: safe on any worker thread
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(url)));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    QByteArray body;
    if (!reply->isFinished()) {
        if (error)
            *error = QStringLiteral("timed out");
        reply->abort();
    } else if (reply->error() != QNetworkReply::NoError) {
        if (error)
            *error = reply->errorString();
    } else {
        body = reply->readAll();
    }
    delete reply;
    return body;
}

} // namespace rl::frigate
