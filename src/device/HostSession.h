#pragma once

#include "core/Database.h"
#include "protocol/BaichuanLink.h"
#include "protocol/ReolinkApi.h"
#include "protocol/ReolinkTransport.h"

#include <QDateTime>
#include <QHash>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

namespace rl {
class ReolinkHttpClient;
}

// A host's session with the app: whether it is logged in, why not, when to try again,
// and whether its polls are healthy. DeviceManager keeps the camera list; this module
// keeps the rules, so they can be exercised without a model, a timer or a network.
namespace rl::device {

// Why a host isn't online. Drives both the sidebar visuals and the retry policy:
// Unreachable is auto-retried with backoff (device off, wrong IP, network down: retrying
// is free); Auth and Locked are NEVER auto-retried, because each rejected login burns the
// firmware's lockout counter.
enum class Problem { None, Connecting, Unreachable, Auth, Locked };

constexpr int kFirstRetrySecs = 15;       // backoff starts here, doubles per failure
constexpr int kMaxRetrySecs = 300;        // ... and stops here
constexpr int kPollFailuresBeforeUnreachable = 3; // ~30 s of failed poll cycles

class HostSession
{
public:
    // ---- login attempts: at most one in flight, retried only when retrying is safe ----
    bool beginValidation();   // false if one is already running
    void endValidation();
    bool validating() const { return m_validating; }

    // The attempt failed for `problem`. Unreachable schedules the next attempt with
    // doubling backoff; Auth and Locked cancel any pending one (the user must act).
    void validationFailed(Problem problem, const QDateTime &now);
    // The HTTP API is still down but a degraded (Baichuan-listed) view is up: keep trying
    // HTTP on the same backoff.
    void keepRetrying(const QDateTime &now);
    void validationSucceeded();
    // The user said "try now" (Reconnect, new credentials): forget the backoff.
    void resetBackoff() { validationSucceeded(); }
    bool retryDue(const QDateTime &now) const;
    std::optional<QDateTime> nextRetryAt() const { return m_nextRetryAt; }
    // Consumes a due retry: true once per scheduled attempt.
    bool takeRetryIfDue(const QDateTime &now);

    // ---- poll health ------------------------------------------------------------------
    bool beginPoll();         // false while the previous poll is still running
    void endPoll();
    enum class PollAction {
        None,
        ProbeAlive,  // ask the Baichuan port whether the device is still there
        Recovered,   // the host answers again after being declared unreachable
        ApiBack,     // the HTTP API answers again after an API-only spell
    };
    // Feed one poll cycle's outcome. Three failures in a row (and every sixth while
    // API-only) ask for a liveness probe; one success after that is a recovery.
    PollAction onPoll(bool transportOk);

    bool beginProbe();        // false if one is already in flight
    enum class ProbeAction {
        Ignore,           // the API came back meanwhile, or nothing changed
        MarkUnreachable,  // the device is gone
        EnterApiOnly,     // API dead, device alive: cameras stay online, API features down
    };
    ProbeAction probeFinished(bool alive);
    bool apiOnly() const { return m_apiOnly; }

private:
    bool m_validating = false;
    bool m_polling = false;
    bool m_probing = false;
    bool m_apiOnly = false;
    int m_pollFails = 0;
    int m_retryDelaySecs = kFirstRetrySecs;
    std::optional<QDateTime> m_nextRetryAt;
};

class HostSessions
{
public:
    HostSession &of(qint64 hostId) { return m_sessions[hostId]; }
    void forget(qint64 hostId) { m_sessions.remove(hostId); }
    // Hosts whose retry is due, each consumed.
    QVector<qint64> takeDueRetries(const QDateTime &now);

private:
    QHash<qint64, HostSession> m_sessions;
};

// ---- the outcome of logging in to a host ---------------------------------------------

struct ChannelResult {
    int channel = 0;
    QString name;
    bool online = false;
    QString codec;  // empty = unknown; the stream is probed instead of assumed
    QSize mainSize; // GetEnc-declared main/sub resolution (for rotation detection)
    QSize subSize;
    int mainKbps = 0; // GetEnc-configured bitrates; 0 = unknown
    int subKbps = 0;
    QString uid;
    api::ChannelCaps caps;
    QString mainRestream; // Frigate: go2rtc stream for HD; empty = the camera's own
    QStringList eventTypes; // the event types it can report; empty = not read
};

// Outcome of a worker-thread validation, applied back on the GUI thread.
struct Validation {
    bool online = false;
    QString status;
    Problem problem = Problem::Unreachable;
    QString hostName; // device/NVR name from GetDevInfo
    QString model;
    int channelNum = 0;
    bool isAdmin = false;
    bool talk = false;
    QString password;
    // A Frigate server's go2rtc restream login, read from the keyring beside the password.
    QString restreamUser;
    QString restreamPassword;
    api::BatteryInfo battery;
    std::shared_ptr<ReolinkHttpClient> client;
    // The HTTP API did not answer but Baichuan listed the cameras: show them (live view
    // works) and keep retrying HTTP for names and the rest.
    bool degraded = false;
    QVector<ChannelResult> channels; // one per camera to show
};

// Logs in to a Reolink host through `http` and reads what the sidebar needs: device info,
// abilities, battery and the camera list (an NVR's online channels, with each one's
// codec). Failure is classified from http.lastFailKind(). When the HTTP API is
// unreachable and `alreadyDegraded` is false, the cameras are listed through a Baichuan
// session from `baichuan` instead (Validation::degraded). Blocking; worker threads only.
// `status` strings are user-visible and translated by the caller's tr context.
Validation validateReolinkHost(ReolinkTransport &http, const HostRecord &rec,
                               const QString &password, bool alreadyDegraded,
                               const BaichuanOpener &baichuan);

// Firmware spells the codec several ways ("h265", "H265", "hevc") and newer builds
// sometimes omit the field entirely. Empty = unknown, and an unknown codec is probed
// from the stream rather than assumed (assuming h264 for an HEVC main stream is issue #4).
QString normalizeCodec(const QString &vtype);

} // namespace rl::device
