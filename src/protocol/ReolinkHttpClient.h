#pragma once

#include "ReolinkApi.h"

#include <QDateTime>
#include <QMutex>
#include <QString>

#include <atomic>
#include <functional>

namespace rl {

// Blocking HTTP-CGI transport for one Reolink host (camera or NVR).
// Owns the login token lifecycle: logs in on demand, refreshes before the
// lease expires, retries once on "please login first". Call from a worker
// thread (DeviceManager wraps calls in QtConcurrent) — never the GUI thread.
//
// Thread-safe, and deliberately SERIAL: one api.cgi request in flight per
// device at a time. The NVR's web server degrades under concurrent commands
// (404/502 bursts — the same behavior reolink_aio works around by putting
// every request behind one per-device lock), and a multi-pane playback page
// fires several Search calls at once. Queueing them here costs a little
// latency; racing them costs whole requests. Bulk clip downloads are exempt —
// they run for minutes and would starve every command behind them.
class ReolinkHttpClient
{
public:
    ReolinkHttpClient(QString host, int port, bool https, QString username, QString password);
    ~ReolinkHttpClient();

    // Why the last exchange failed. Callers need the distinction because the
    // right reaction differs: Transport (device off/wrong address) is safe to
    // retry; Auth/Locked must NOT be auto-retried — every rejected login burns
    // the firmware's 10-attempt counter toward locking the account out.
    enum class FailKind { None, Transport, Auth, Locked, Protocol };
    FailKind lastFailKind();

    // Sends a batch of commands (api::command(...)), handling login transparently.
    api::BatchResult call(const Json &commands);

    // A command the device answers slowly (NvrDownload cuts the clip before replying:
    // about 1.6 s per minute of footage). Not held behind the one-at-a-time lock, so it
    // cannot freeze playback searches on the same NVR while it runs; `timeoutSec` is the
    // cap for the reply and `cancel`, when set, aborts the wait promptly.
    api::BatchResult callSlow(const Json &commands, long timeoutSec,
                              const std::atomic<bool> *cancel = nullptr);

    // Convenience for a single command; returns the lone CommandResult.
    api::CommandResult callOne(const QString &cmd, Json param = Json::object(), int action = 0);

    // Fetch a JPEG snapshot of a channel (cmd=Snap, binary GET). Returns empty on
    // failure with *error set.
    QByteArray fetchSnapshot(int channel, QString *error = nullptr);

    // Stream an authenticated GET straight to a file (e.g. a recording clip via
    // cmd=Download). Streams to disk rather than buffering, so it handles the large
    // clips the endpoint produces. Returns false with *error set on failure (and
    // removes any partial file). timeoutSec allows a long cap (0 = default).
    //
    // Long downloads: pass `stallSec` > 0 to drop the overall time cap and instead give
    // up only after that many seconds without meaningful data (a slow link is fine, a dead
    // one is not). `progress(done, total)` is called as bytes arrive (total 0 if unknown);
    // setting `cancel` aborts promptly and removes the partial file.
    bool downloadToFile(const QString &url, const QString &destPath, long timeoutSec = 0,
                        QString *error = nullptr,
                        const std::function<void(qint64 done, qint64 total)> &progress = {},
                        long stallSec = 0, const std::atomic<bool> *cancel = nullptr);

    bool ensureLogin(QString *error = nullptr);
    void logout();

    QString host() const { return m_host; }
    // Current session token (may be empty if not logged in / expired). Thread-safe.
    QString token();

private:
    struct HttpResponse {
        bool ok = false;
        long status = 0;
        QByteArray body;
        QString error;
        QByteArray contentType;
    };
    // totalTimeoutSec == 0 uses the default; pass a smaller value for best-effort
    // calls (e.g. Logout) that must not pin a thread on a dead device.
    HttpResponse post(const QString &url, const QByteArray &body, long totalTimeoutSec = 0,
                      const std::atomic<bool> *cancel = nullptr);
    api::BatchResult callImpl(const Json &commands, long timeoutSec, bool serial,
                              const std::atomic<bool> *cancel);
    // totalTimeoutSec == 0 uses the default; large clip downloads from a slow NVR
    // need a generous value.
    HttpResponse get(const QString &url, long totalTimeoutSec = 0);
    bool loginLocked(QString *error);
    bool tokenValidLocked() const;

    QString m_host;
    int m_port;
    bool m_https;
    QString m_username;
    QString m_password;

    // Lock order: m_requestMutex (outer, held across a whole HTTP exchange)
    // then m_mutex (inner, brief). Never the reverse.
    void setFailKind(FailKind kind);

    QMutex m_requestMutex; // serializes api.cgi requests to this device
    QMutex m_mutex;        // guards token state
    QString m_token;
    QDateTime m_tokenExpiry;
    FailKind m_failKind = FailKind::None;
};

} // namespace rl
