#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

// Download a time range of recorded footage over Baichuan (TCP 9000): cmd 143 asks the
// recorder for [start, end] of one channel, the footage comes back as more 143 messages
// carrying binary media (the same BCMedia blocks as live view), a 143 with status 300
// (or 331, or an empty one) marks the end, and 144 closes the download (sent early, it
// cancels). This is what Reolink's own clients use for clip export; NVR firmware
// v3.6.5.597 broke the HTTP NvrDownload fragments.
//
// An NVR serves one such download at a time, also across channels; callers queue.
//
// Clean-room from MIT reolink_aio (PR #241) and the RLN36 measurements in reolink_aio#240.
namespace rl {

class BaichuanControl;

struct BcDownloadRequest {
    int channel = 0;
    QString uid;          // camera UID; omitted from the request when empty
    QDateTime start;      // the recorder's local time, as the rest of the app assumes
    QDateTime end;
    bool subStream = false; // export is full quality; reverse play fetches the sub stream
};

// The cmd 143 request body.
QByteArray bcDownloadXml(const BcDownloadRequest &req);

// One logged-in Baichuan session, as the download needs it. The real one wraps
// BaichuanControl; tests script it. Blocking, worker thread only.
class BcDownloadPort
{
public:
    struct Message {
        quint32 cmdId = 0;
        quint16 status = 0;
        bool binary = false;   // the Extension says <binaryData>1</binaryData>
        QByteArray body;       // media bytes (already decrypted) or the reply XML
    };
    virtual ~BcDownloadPort() = default;
    // Send `bodyXml` (empty = header only) to `cmdId` on `channel`. False if the link is gone.
    virtual bool send(quint32 cmdId, int channel, const QByteArray &bodyXml) = 0;
    // The next message, waiting up to `waitMs`; nothing on timeout. *lost = the link is gone.
    virtual std::optional<Message> next(int waitMs, bool *lost) = 0;
};

// The real port: an open (logged-in) BaichuanControl. One message id for the whole
// download, so the 144 that closes it names the same request as the 143.
std::unique_ptr<BcDownloadPort> bcDownloadPort(BaichuanControl &control);

struct BcDownloadResult {
    enum class Outcome {
        Done,      // the recorder said the range is complete
        NoData,    // it ended without sending any footage
        Refused,   // it turned the request down (see status; 400 for an empty channel)
        Failed,    // it sent an error part way through (see status)
        Cancelled,
        Stalled,   // nothing arrived for too long
        Lost,      // the connection dropped
    };
    Outcome outcome = Outcome::Lost;
    quint16 status = 0;     // the status that ended it (Refused, Done)
    qint64 mediaBytes = 0;
    bool ok() const { return outcome == Outcome::Done; }
};

struct BcDownloadOptions {
    const std::atomic<bool> *cancel = nullptr;
    int stallMs = 30000;
    // Every non-media 143/144 message, for logs and probes.
    std::function<void(const BcDownloadPort::Message &)> trace;
    // Called once the recorder accepts, with the size it announces (sizeL + sizeH << 32;
    // -1 when it gives none or a nonsense one: it is sometimes -1 or wrong). Return false
    // to cancel before any footage is taken, e.g. when the disk is too small.
    std::function<bool(qint64 announcedBytes)> onAccepted;
};

constexpr quint32 kBcCmdDownload = 143;
constexpr quint32 kBcCmdDownloadStop = 144;

// Run one download to completion, handing each media payload to `media` in order.
// Closes the download with 144 whenever it ends on our side or the recorder's (done,
// error, cancel, stall), but not after a refusal: the recorder has nothing open then, and
// a 144 could stop another client's download.
BcDownloadResult runBcDownload(BcDownloadPort &port, const BcDownloadRequest &req,
                               const std::function<void(const QByteArray &)> &media,
                               const BcDownloadOptions &opts = {});

} // namespace rl
