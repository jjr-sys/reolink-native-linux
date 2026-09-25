#include "AudioEventApi.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <algorithm>

namespace rl::audio {

static QString str(const Json &o, const char *key)
{
    return o.contains(key) && o[key].is_string() ? QString::fromStdString(o[key].get<std::string>())
                                                 : QString();
}

// Raised-voice scores are {"Shout": 0.61, ...}: show the strongest. Threat-speech scores are
// {"phrases": 2, "categories": ["threat"]}: show the count and categories.
static QString scoreTextFor(const Json &scores)
{
    if (!scores.is_object())
        return {};
    QStringList parts;
    QString best;
    double bestV = -1;
    for (auto it = scores.begin(); it != scores.end(); ++it) {
        if (it.value().is_number() && it.key() != "phrases" && it.value().get<double>() > bestV) {
            bestV = it.value().get<double>();
            best = QString::fromStdString(it.key());
        }
    }
    if (bestV >= 0)
        parts << QStringLiteral("%1 %2%").arg(best).arg(qRound(bestV * 100));
    if (scores.contains("phrases") && scores["phrases"].is_number())
        parts << QStringLiteral("%1 phrase(s)").arg(scores["phrases"].get<int>());
    if (scores.contains("categories") && scores["categories"].is_array()) {
        QStringList cats;
        for (const Json &c : scores["categories"])
            if (c.is_string())
                cats << QString::fromStdString(c.get<std::string>());
        if (!cats.isEmpty())
            parts << cats.join(QStringLiteral(", "));
    }
    return parts.join(QStringLiteral(" · "));
}

QVector<Event> parseEvents(const Json &events)
{
    QVector<Event> out;
    if (!events.is_array())
        return out;
    for (const Json &e : events) {
        if (!e.is_object() || !e.contains("time") || !e["time"].is_number())
            continue;
        Event ev;
        ev.id = str(e, "id");
        ev.camera = str(e, "camera");
        if (ev.id.isEmpty() || ev.camera.isEmpty())
            continue;
        ev.time = e["time"].get<double>();
        if (e.contains("triggers") && e["triggers"].is_array())
            for (const Json &t : e["triggers"])
                if (t.is_string())
                    ev.triggers << QString::fromStdString(t.get<std::string>());
        ev.scoreText = e.contains("scores") ? scoreTextFor(e["scores"]) : QString();
        ev.transcript = str(e, "transcript"); // null -> empty
        ev.hasClip = e.contains("has_clip") && e["has_clip"].is_boolean() && e["has_clip"].get<bool>();
        out.append(ev);
    }
    return out;
}

Status parseStatus(const Json &status)
{
    Status s;
    if (!status.is_object() || !status.contains("transcription_paused")
        || !status["transcription_paused"].is_boolean())
        return s;
    s.ok = true;
    s.transcriptionPaused = status["transcription_paused"].get<bool>();
    s.pausedReason = str(status, "paused_reason");
    return s;
}

QString triggerText(const QStringList &triggers)
{
    QStringList parts;
    if (triggers.contains(QLatin1String(kRaisedVoice)))
        parts << QObject::tr("Loud voice");
    if (triggers.contains(QLatin1String(kThreatSpeech)))
        parts << QObject::tr("Threatening words");
    if (parts.isEmpty())
        return triggers.contains(QLatin1String(kSpeech)) ? QObject::tr("Speech") : QObject::tr("Audio event");
    return parts.join(QStringLiteral(" + "));
}

static QString base(const QString &host, int port)
{
    return QStringLiteral("http://%1:%2").arg(host).arg(port);
}
QString eventsUrl(const QString &host, int port, int limit, double before, bool flaggedOnly)
{
    QString url = base(host, port) + QStringLiteral("/events?limit=%1").arg(limit);
    if (before > 0)
        url += QStringLiteral("&before=%1").arg(QString::number(before, 'f', 3));
    if (flaggedOnly)
        url += QStringLiteral("&flagged=1");
    return url;
}
QString statusUrl(const QString &host, int port) { return base(host, port) + QStringLiteral("/status"); }
QString resumeUrl(const QString &host, int port) { return base(host, port) + QStringLiteral("/resume"); }
QString clipUrl(const QString &host, int port, const QString &eventId)
{
    return base(host, port) + QStringLiteral("/events/") + QString::fromUtf8(QUrl::toPercentEncoding(eventId))
           + QStringLiteral("/clip");
}

static Reply request(const QString &url, const QString &token, int timeoutMs, bool post)
{
    Reply r;
    QNetworkAccessManager nam; // thread-local: safe on any worker thread
    QNetworkRequest req{QUrl(url)};
    if (!token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    QNetworkReply *reply = post ? nam.post(req, QByteArray()) : nam.get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    if (!reply->isFinished()) {
        r.error = QStringLiteral("timed out");
        reply->abort();
    } else {
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        if (r.status == 0)
            r.error = reply->errorString();
    }
    delete reply;
    return r;
}
Reply httpGet(const QString &url, const QString &token, int timeoutMs) { return request(url, token, timeoutMs, false); }
Reply httpPost(const QString &url, const QString &token, int timeoutMs) { return request(url, token, timeoutMs, true); }

// ---- AlertTracker ---------------------------------------------------------------------

QVector<Event> AlertTracker::update(const QVector<Event> &events, double now)
{
    QVector<Event> fresh;
    for (int i = events.size() - 1; i >= 0; --i) { // oldest first
        const Event &e = events.at(i);
        if (m_seen.contains(e.id))
            continue;
        m_seen.insert(e.id);
        if (!e.flagged())
            continue; // plain speech is stored, not alerted
        // Backlog events keep their own time; live ones count from arrival.
        const double at = m_primed ? now : e.time;
        const double until = at + kQuietSecs;
        if (until > now)
            m_until[e.camera] = std::max(m_until.value(e.camera, 0.0), until);
        if (m_primed)
            fresh.append(e);
    }
    // The server lists at most its newest events; one that dropped off never comes back.
    QSet<QString> current;
    for (const Event &e : events)
        current.insert(e.id);
    m_seen = current;
    m_primed = true;
    return fresh;
}

bool AlertTracker::active(const QString &camera, double now) const
{
    return m_until.value(camera, 0.0) > now;
}

QStringList AlertTracker::activeCameras(double now) const
{
    QStringList out;
    for (auto it = m_until.begin(); it != m_until.end(); ++it)
        if (it.value() > now)
            out << it.key();
    return out;
}

double AlertTracker::nextExpiry(double now) const
{
    double soonest = 0;
    for (double until : m_until)
        if (until > now && (soonest == 0 || until < soonest))
            soonest = until;
    return soonest;
}

} // namespace rl::audio
