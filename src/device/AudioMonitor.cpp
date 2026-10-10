#include "AudioMonitor.h"

#include "core/Log.h"

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QtConcurrent>

namespace rl {

namespace {
constexpr const char *kIcon = "io.github.todesengelx.ReolinkLinux";
constexpr int kMaxEvents = 100; // the service never lists more

double nowSecs() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }
} // namespace

AudioMonitor::AudioMonitor(CredentialStore *credentials, QObject *parent)
    : QAbstractListModel(parent), m_credentials(credentials), m_clipDir()
{
    qRegisterMetaType<QVector<audio::Event>>();
    qRegisterMetaType<audio::Status>();

    QSettings s;
    m_host = s.value(QStringLiteral("audio/host"), QStringLiteral("woorabinda.tail359f9.ts.net")).toString();
    m_port = s.value(QStringLiteral("audio/port"), 8090).toInt();
    m_notify = s.value(QStringLiteral("audio/notify"), true).toBool();

    bool ok = false;
    m_token = m_credentials->lookup(kTokenHostId, &ok);
    if (!ok)
        m_token.clear();

    m_expiryTimer.setSingleShot(true);
    connect(&m_expiryTimer, &QTimer::timeout, this, &AudioMonitor::onExpiry);

    // RL_MOCK_AUDIO_EVENTS seeds the model and a live alert without a server, so the UI
    // can be checked. Nothing is written anywhere.
    if (qEnvironmentVariableIsSet("RL_MOCK_AUDIO_EVENTS")) {
        const double now = nowSecs();
        audio::Event a;
        a.id = QStringLiteral("mock-1");
        a.time = now - 30;
        a.camera = QStringLiteral("kitchen");
        a.triggers = {audio::kThreatSpeech};
        a.scoreText = QStringLiteral("2 phrase(s) · threat");
        a.transcript = QStringLiteral("(mock transcript)");
        audio::Event b;
        b.id = QStringLiteral("mock-2");
        b.time = now - 3600;
        b.camera = QStringLiteral("kitchen");
        b.triggers = {audio::kRaisedVoice};
        b.scoreText = QStringLiteral("Shout 61%");
        b.hasClip = true;
        audio::Event c;
        c.id = QStringLiteral("mock-3");
        c.time = now - 120;
        c.camera = QStringLiteral("kitchen");
        c.triggers = {audio::kSpeech};
        c.transcript = QStringLiteral("(mock plain speech, kept but not an alert)");
        c.hasClip = true;
        m_events = {a, c, b};
        m_tracker.update(m_events, now); // plain speech is ignored by the tracker
        m_status.ok = true;
        m_status.transcriptionPaused = qEnvironmentVariable("RL_MOCK_AUDIO_EVENTS") == QLatin1String("paused");
        m_status.pausedReason = QStringLiteral("sustained_load load1=4.2");
        m_reachable = true;
        m_token = QStringLiteral("mock");
        onExpiry();
        return;
    }

    if (!enabled()) {
        qCInfo(lcCore) << "audio monitor: no token in the keyring, disabled";
        return;
    }
    m_pollTimer.setInterval(kPollMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &AudioMonitor::poll);
    m_pollTimer.start();
    QTimer::singleShot(0, this, &AudioMonitor::poll);
}

AudioMonitor::~AudioMonitor()
{
    m_pollTimer.stop();
    m_pending.waitForFinished();
}

int AudioMonitor::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_events.size();
}

QVariant AudioMonitor::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_events.size())
        return {};
    const audio::Event &e = m_events.at(index.row());
    switch (role) {
    case IdRole: return e.id;
    case TimestampRole: return static_cast<qint64>(e.time);
    case TimeTextRole: {
        const QDateTime dt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(e.time));
        const qint64 ago = QDateTime::currentSecsSinceEpoch() - static_cast<qint64>(e.time);
        if (ago < 60)
            return tr("just now");
        if (ago < 3600)
            return tr("%1 min ago").arg(ago / 60);
        if (dt.date() == QDate::currentDate())
            return dt.toString(QStringLiteral("hh:mm"));
        return dt.toString(QStringLiteral("MMM d, hh:mm"));
    }
    case CameraRole: return e.camera.left(1).toUpper() + e.camera.mid(1);
    case TriggerTextRole: return audio::triggerText(e.triggers);
    case TriggersRole: return e.triggers;
    case ScoreTextRole: return e.scoreText;
    case TranscriptRole: return e.transcript;
    case HasClipRole: return e.hasClip;
    case FlaggedRole: return e.flagged();
    case StartRole: return static_cast<qint64>(e.start);
    case EndRole: return static_cast<qint64>(e.end);
    case RawCameraRole: return e.camera;
    // "model" is avoided as a role name: QML delegates already have an implicit `model`
    // context property, so a role of that name would be unreachable/shadowed.
    case SttModelRole: return audio::modelDisplayName(e.model);
    case MegaAsrTranscriptRole: return e.megaAsrTranscript;
    case MegaAsrModelRole: return e.megaAsrTranscript.isEmpty() ? QString()
                                                                 : audio::modelDisplayName(QStringLiteral("mega-asr"));
    }
    return {};
}

QHash<int, QByteArray> AudioMonitor::roleNames() const
{
    return {{IdRole, "eventId"},        {TimestampRole, "timestamp"}, {TimeTextRole, "timeText"},
            {CameraRole, "camera"},     {TriggerTextRole, "triggerText"}, {TriggersRole, "triggers"},
            {ScoreTextRole, "scoreText"}, {TranscriptRole, "transcript"}, {HasClipRole, "hasClip"}, {FlaggedRole, "flagged"},
            {StartRole, "startTime"}, {EndRole, "endTime"}, {RawCameraRole, "cameraKey"},
            {SttModelRole, "sttModel"}, {MegaAsrTranscriptRole, "megaAsrTranscript"}, {MegaAsrModelRole, "megaAsrModel"}};
}

void AudioMonitor::setNotificationsEnabled(bool on)
{
    if (m_notify == on)
        return;
    m_notify = on;
    QSettings().setValue(QStringLiteral("audio/notify"), on);
    emit notificationsEnabledChanged();
}

bool AudioMonitor::isAlerting(const QString &cameraName) const
{
    const QString key = keyOf(cameraName);
    if (key.isEmpty())
        return false;
    return m_tracker.active(key, nowSecs());
}

void AudioMonitor::poll()
{
    if (m_polling || !enabled())
        return;
    m_polling = true;
    const QString host = m_host, token = m_token;
    const int port = m_port;
    const bool all = m_showAll;
    const int limit = m_limit;
    QPointer<AudioMonitor> self(this);
    m_pending.addFuture(QtConcurrent::run([self, host, port, token, all, limit] {
        auto fetch = [&](int n, bool flaggedOnly, bool *ok) {
            const auto r = audio::httpGet(audio::eventsUrl(host, port, n, 0, flaggedOnly), token, 8000);
            *ok = r.status == 200;
            return *ok ? audio::parseEvents(Json::parse(r.body.toStdString(), nullptr, false))
                       : QVector<audio::Event>();
        };
        bool haveAlerts = false, haveListing = false;
        const auto alerts = fetch(100, true, &haveAlerts); // drives borders and notifications
        const auto listing = all ? fetch(limit, false, &haveListing) : alerts;
        haveListing = all ? haveListing : haveAlerts;
        const auto st = audio::httpGet(audio::statusUrl(host, port), token, 8000);
        const Json stJson = st.status == 200 ? Json::parse(st.body.toStdString(), nullptr, false) : Json();
        const audio::Status status = audio::parseStatus(stJson);
        const bool reachable = haveAlerts || st.status == 200;
        if (self)
            QMetaObject::invokeMethod(self.data(), "onPolled", Qt::QueuedConnection,
                                      Q_ARG(bool, reachable), Q_ARG(QVector<rl::audio::Event>, alerts),
                                      Q_ARG(bool, haveAlerts), Q_ARG(QVector<rl::audio::Event>, listing),
                                      Q_ARG(bool, haveListing), Q_ARG(rl::audio::Status, status));
    }));
}

void AudioMonitor::setShowAll(bool on)
{
    if (m_showAll == on)
        return;
    m_showAll = on;
    m_limit = kPageSize;
    emit showAllChanged();
    poll();
}

void AudioMonitor::loadMore()
{
    if (!canLoadMore())
        return;
    m_limit = qMin(kMaxLimit, m_limit + kPageSize);
    poll();
}

void AudioMonitor::onPolled(bool reachable, const QVector<audio::Event> &alerts, bool haveAlerts,
                            const QVector<audio::Event> &listing, bool haveListing,
                            const audio::Status &status)
{
    m_polling = false;
    bool statusDirty = false;
    if (reachable != m_reachable) {
        qCInfo(lcCore) << "audio monitor:" << (reachable ? "reachable" : "not reachable") << "-"
                       << alerts.size() << "alerts";
        m_reachable = reachable;
        statusDirty = true;
    }
    if (status.ok && (status.transcriptionPaused != m_status.transcriptionPaused
                      || status.pausedReason != m_status.pausedReason || !m_status.ok)) {
        m_status = status;
        statusDirty = true;
        if (status.transcriptionPaused && !m_pauseNotified) {
            m_pauseNotified = true;
            if (m_notify)
                m_notifier.notify(tr("Audio monitor"), tr("Transcription paused: server is busy"),
                                  QString::fromLatin1(kIcon));
        } else if (!status.transcriptionPaused) {
            m_pauseNotified = false;
        }
    }
    if (statusDirty)
        emit statusChanged();

    if (haveAlerts) {
        const QVector<audio::Event> fresh = m_tracker.update(alerts, nowSecs());
        for (const audio::Event &e : fresh)
            notifyFor(e);
        if (!fresh.isEmpty() || !m_primedBorders) {
            m_primedBorders = true;
            onExpiry();
        }
    }
    if (haveListing && (listing.size() != m_events.size()
                        || (!listing.isEmpty() && !m_events.isEmpty() && listing.first().id != m_events.first().id))) {
        beginResetModel();
        m_events = listing.mid(0, kMaxLimit);
        endResetModel();
        emit countChanged();
    }
}

void AudioMonitor::notifyFor(const audio::Event &e)
{
    if (!m_notify)
        return;
    const QString cam = e.camera.left(1).toUpper() + e.camera.mid(1);
    const bool threat = e.triggers.contains(QLatin1String(audio::kThreatSpeech));
    const bool loud = e.triggers.contains(QLatin1String(audio::kRaisedVoice));
    const QString what = threat && loud ? tr("Loud voice and threatening words detected")
                         : threat        ? tr("Threatening words detected")
                                         : tr("Loud voice detected");
    m_notifier.notify(cam, what, QString::fromLatin1(kIcon));
}

// Re-evaluates borders and schedules the next moment one will clear.
void AudioMonitor::onExpiry()
{
    ++m_revision;
    emit alertsChanged();
    const double now = nowSecs();
    const double next = m_tracker.nextExpiry(now);
    if (next > 0)
        m_expiryTimer.start(static_cast<int>((next - now) * 1000) + 250);
}

void AudioMonitor::resumeTranscription()
{
    if (!enabled())
        return;
    const QString host = m_host, token = m_token;
    const int port = m_port;
    QPointer<AudioMonitor> self(this);
    m_pending.addFuture(QtConcurrent::run([self, host, port, token] {
        const auto r = audio::httpPost(audio::resumeUrl(host, port), token, 8000);
        if (r.status != 200)
            qCWarning(lcCore) << "audio monitor: resume failed, status" << r.status << r.error;
        if (self)
            QMetaObject::invokeMethod(self.data(), "poll", Qt::QueuedConnection);
    }));
}

void AudioMonitor::fetchClip(const QString &eventId)
{
    if (!enabled() || !m_clipDir.isValid())
        return;
    const QString host = m_host, token = m_token, dir = m_clipDir.path();
    const int port = m_port;
    QPointer<AudioMonitor> self(this);
    m_pending.addFuture(QtConcurrent::run([self, host, port, token, dir, eventId] {
        const auto r = audio::httpGet(audio::clipUrl(host, port, eventId), token, 20000);
        QString path, error;
        if (r.status == 200 && !r.body.isEmpty()) {
            // The id comes from the server; keep only what is safe in a file name.
            QString safe = eventId;
            safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
            // Clips are Opus in Ogg (older ones WAV); the container tells them apart.
            const bool ogg = r.body.startsWith("OggS");
            path = dir + QLatin1Char('/') + safe + (ogg ? QStringLiteral(".ogg") : QStringLiteral(".wav"));
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(r.body) != r.body.size()) {
                error = QObject::tr("could not save the clip");
                path.clear();
            }
        } else if (r.status == 404) {
            error = QObject::tr("This clip has expired (clips are kept for 7 days).");
        } else {
            error = QObject::tr("Could not fetch the clip from the audio server.");
        }
        if (self)
            QMetaObject::invokeMethod(self.data(), [self, eventId, path, error] {
                if (self)
                    emit self->clipReady(eventId, path, error);
            }, Qt::QueuedConnection);
    }));
}

void AudioMonitor::discardClips()
{
    const QDir dir(m_clipDir.path());
    for (const QString &f : dir.entryList({QStringLiteral("*.wav"), QStringLiteral("*.ogg")}, QDir::Files))
        QFile::remove(dir.filePath(f));
}

} // namespace rl
