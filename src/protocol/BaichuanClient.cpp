#include "BaichuanClient.h"
#include "media/FrameTimeSei.h"
#include "media/ReversePlay.h"
#include "protocol/BaichuanControl.h"
#include "protocol/BaichuanDownload.h"
#include "core/Log.h"

#include "protocol/BcCrypto.h"
#include "protocol/BcLiveFramePacer.h"
#include "protocol/BcReplay.h"

#include <QDateTime>

#include <algorithm>
#include <QElapsedTimer>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>

extern "C" {
#include <libavutil/error.h>
}

namespace rl {

namespace {

constexpr quint32 kMagic = 0x0ABCDEF0;
constexpr quint32 kMagicRev = 0x0FEDCBA0;
constexpr int kMaxRing = 8 * 1024 * 1024; // backpressure cap for the decoded buffer

void putLE32(QByteArray &b, quint32 v)
{
    char t[4];
    qToLittleEndian(v, t);
    b.append(t, 4);
}
void putLE16(QByteArray &b, quint16 v)
{
    char t[2];
    qToLittleEndian(v, t);
    b.append(t, 2);
}
quint32 getLE32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}
quint16 getLE16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

struct Msg {
    quint32 id = 0;
    quint8 channel = 0;
    quint16 msgNum = 0;
    quint16 code = 0;
    quint16 cls = 0;
    quint32 payloadOffset = 0;
    QByteArray body;
};

// Serialize one BC message. `cls` selects legacy(0x6514, 20-byte) vs modern.
QByteArray frame(quint32 id, quint8 ch, quint8 streamType, quint16 msgNum, quint16 code,
                 quint16 cls, const QByteArray &body)
{
    const bool hasPayloadOffset = (cls == 0x6414 || cls == 0x0000);
    QByteArray m;
    putLE32(m, kMagic);
    putLE32(m, id);
    putLE32(m, static_cast<quint32>(body.size()));
    m.append(static_cast<char>(ch));
    m.append(static_cast<char>(streamType));
    putLE16(m, msgNum);
    putLE16(m, code);
    putLE16(m, cls);
    if (hasPayloadOffset)
        putLE32(m, 0); // no Extension on anything we send
    m.append(body);
    return m;
}

// Pull one complete message from `buf` (consuming it). False if incomplete.
bool parseMessage(QByteArray &buf, Msg &m)
{
    if (buf.size() < 20)
        return false;
    const quint32 magic = getLE32(buf.constData());
    if (magic != kMagic && magic != kMagicRev)
        return false; // caller resyncs
    const quint32 bodyLen = getLE32(buf.constData() + 8);
    const quint16 cls = getLE16(buf.constData() + 18);
    const bool hasPayloadOffset = (cls == 0x6414 || cls == 0x0000);
    const int headerLen = hasPayloadOffset ? 24 : 20;
    if (buf.size() < headerLen + static_cast<int>(bodyLen))
        return false;
    m.id = getLE32(buf.constData() + 4);
    m.channel = static_cast<quint8>(buf[12]);
    m.msgNum = getLE16(buf.constData() + 14);
    m.code = getLE16(buf.constData() + 16);
    m.cls = cls;
    m.payloadOffset = hasPayloadOffset ? getLE32(buf.constData() + 20) : 0;
    m.body = buf.mid(headerLen, static_cast<int>(bodyLen));
    buf.remove(0, headerLen + static_cast<int>(bodyLen));
    return true;
}

} // namespace

BaichuanClient::BaichuanClient(Params params) : m_p(std::move(params)) {}

BaichuanClient::~BaichuanClient()
{
    stop();
    // Join (not detach): the worker touches our members, so it must finish before
    // they are destroyed. It never holds a reference to us, so this can't
    // self-deadlock, and every network wait is short + abort-checked, so the join
    // is bounded (sub-second). The last owning reference is dropped by the
    // StreamPlayer decode worker, so this runs off the GUI thread in practice.
    if (m_thread.joinable())
        m_thread.join();
}

void BaichuanClient::start()
{
    if (m_started)
        return;
    m_started = true;
    m_thread = std::thread([this] { run(); });
}

void BaichuanClient::stop()
{
    m_abort.store(true);
    QMutexLocker lock(&m_mutex);
    m_finished = true;
    m_cond.wakeAll();
}

QString BaichuanClient::ffmpegFormat()
{
    QMutexLocker lock(&m_mutex);
    return m_format;
}

int BaichuanClient::read(unsigned char *buf, int size)
{
    QMutexLocker lock(&m_mutex);
    while (m_ring.isEmpty() && !m_finished && !m_abort.load())
        m_cond.wait(&m_mutex);
    if (m_abort.load() && m_ring.isEmpty())
        return AVERROR_EOF;
    if (m_ring.isEmpty())
        return !m_finished ? 0 : m_failed ? AVERROR(EIO) : AVERROR_EOF;
    const int n = qMin(size, static_cast<int>(m_ring.size()));
    memcpy(buf, m_ring.constData(), n);
    m_ring.remove(0, n);
    m_cond.wakeAll(); // unblock the producer if it was backpressured
    return n;
}

void BaichuanClient::pushAnnexB(const QByteArray &bytes)
{
    QMutexLocker lock(&m_mutex);
    // Backpressure: block the producer if the reader falls far behind.
    while (m_ring.size() > kMaxRing && !m_abort.load())
        m_cond.wait(&m_mutex);
    if (m_abort.load())
        return;
    m_ring.append(bytes);
    m_cond.wakeAll();
}

void BaichuanClient::run()
{
    if (m_p.reverse && m_p.startEpoch > 0) {
        runReverse();
    } else {
        QTcpSocket sock;
        QByteArray aesKey;
        if (connectAndLogin(sock, aesKey) && !m_abort.load()) {
            if (startStream(sock, aesKey))
                pumpMedia(sock, aesKey, /*streamMsgNum=*/20);
        }
    }
    QMutexLocker lock(&m_mutex);
    m_finished = true;
    m_cond.wakeAll();
}

bool BaichuanClient::connectAndLogin(QTcpSocket &sock, QByteArray &aesKey)
{
    sock.connectToHost(m_p.host, static_cast<quint16>(m_p.port));
    if (!sock.waitForConnected(5000))
        return false;

    QByteArray netBuf;
    auto recvMatch = [&](quint16 wantMsgNum, Msg &out) -> bool {
        // Short per-wait timeout so abort (destruction) is honored within ~0.3s;
        // the loop count still allows several seconds total for a slow login.
        for (int i = 0; i < 40 && !m_abort.load(); ++i) {
            while (parseMessage(netBuf, out))
                if (out.msgNum == wantMsgNum)
                    return true;
            if (!sock.waitForReadyRead(300))
                continue;
            netBuf.append(sock.readAll());
        }
        return false;
    };

    // Step 1: legacy-upgrade requesting AES (this firmware refuses BCEncrypt).
    // The login handshake is connection-level, ALWAYS channel 0 — the NVR rejects
    // a login framed with a camera channel. The camera is selected later by the
    // stream request (header channel byte + <channelId> in its XML).
    sock.write(frame(1, 0, 0, 1, 0xDC12, 0x6514, QByteArray()));
    sock.flush();
    Msg nonceMsg;
    if (!recvMatch(1, nonceMsg))
        return false;
    const QByteArray nonceXml = bc::xorCrypt(nonceMsg.body, nonceMsg.channel);
    const int a = nonceXml.indexOf("<nonce>");
    const int z = nonceXml.indexOf("</nonce>");
    if (a < 0 || z < 0)
        return false;
    const QString nonce = QString::fromUtf8(nonceXml.mid(a + 7, z - a - 7));
    aesKey = bc::aesKey(nonce, m_p.password);

    // Step 2: modern login (XOR body — msg_id 1 is always XOR).
    const QByteArray user = bc::modernHash(m_p.username, nonce);
    const QByteArray pass = bc::modernHash(m_p.password, nonce);
    QByteArray login = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<LoginUser version=\"1.1\">\n"
                       "<userName>" + user + "</userName>\n<password>" + pass +
                       "</password>\n<userVer>1</userVer>\n</LoginUser>\n"
                       "<LoginNet version=\"1.1\"><type>LAN</type><udpPort>0</udpPort></LoginNet>\n</body>\n";
    sock.write(frame(1, 0, 0, 1, 0, 0x6414, bc::xorCrypt(login, 0)));
    sock.flush();
    Msg loginReply;
    if (!recvMatch(1, loginReply))
        return false;
    // Carry any leftover bytes into the media pump.
    m_loginTail = netBuf;
    return loginReply.code == 200;
}

bool BaichuanClient::startStream(QTcpSocket &sock, const QByteArray &aesKey)
{
    const quint8 streamByte = m_p.mainStream ? 0 : 1;
    const QString streamName = m_p.mainStream ? QStringLiteral("mainStream")
                                              : QStringLiteral("subStream");
    QByteArray netBuf = m_loginTail;

    if (m_p.startEpoch <= 0) {
        // Live preview: msg_id 3.
        QByteArray preview =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body><Preview version=\"1.1\">\n"
            "<channelId>" + QByteArray::number(m_p.channel) + "</channelId>\n<handle>" +
            QByteArray::number(m_p.mainStream ? 0 : 256) + "</handle>\n<streamType>" +
            streamName.toUtf8() + "</streamType>\n</Preview></body>\n";
        sock.write(frame(3, m_p.channel, streamByte, 20, 0, 0x6414, bc::aesCfb(preview, aesKey, false)));
        sock.flush();
        m_streamTail = netBuf;
        return true;
    }

    // Recorded playback, paced by the recorder (cmd 381). Media follows on msg_num 20.
    sendReplayStart(sock, aesKey, m_p.startEpoch, wantedReplaySpeed());
    m_streamTail = netBuf;
    return true;
}

int BaichuanClient::wantedReplaySpeed() const
{
    return bc::recorderSpeed(m_speed ? m_speed->load(std::memory_order_relaxed) : 1.0);
}

// A request on this client's stream, in the playback framing (header channel = camera,
// msg_num 20, no Extension), with an AES body (empty = header only).
void BaichuanClient::sendOnStream(QTcpSocket &sock, const QByteArray &aesKey, quint32 cmd,
                                  const QByteArray &xml)
{
    sock.write(frame(cmd, m_p.channel, m_p.mainStream ? 0 : 1, 20, 0, 0x6414,
                     xml.isEmpty() ? QByteArray() : bc::aesCfb(xml, aesKey, false)));
    sock.flush();
}

void BaichuanClient::sendReplayStart(QTcpSocket &sock, const QByteArray &aesKey, qint64 startEpoch,
                                     int speed)
{
    const QDateTime start = QDateTime::fromSecsSinceEpoch(startEpoch);
    m_replayDay = start.date();
    m_replaySpeed = speed;
    sendOnStream(sock, aesKey, bc::kReplayStart, bc::replayStartXml(m_p.channel, m_p.mainStream, speed, start));
}

void BaichuanClient::sendReplaySeek(QTcpSocket &sock, const QByteArray &aesKey, qint64 toEpoch)
{
    sendOnStream(sock, aesKey, bc::kReplaySeek,
                 bc::replaySeekXml(m_p.channel, m_p.mainStream, QDateTime::fromSecsSinceEpoch(toEpoch)));
}

void BaichuanClient::sendReplayStop(QTcpSocket &sock, const QByteArray &aesKey)
{
    sendOnStream(sock, aesKey, bc::kReplayStop, QByteArray());
}

// Request the day's footage from `startEpoch` by time (cmd 143, a download: unpaced).
// Structure verified across independent PCAP-derived implementations: a
// FileInfoList/FileInfo with the channel, stream, and a start/end time range.
void BaichuanClient::sendDownloadByTime(QTcpSocket &sock, const QByteArray &aesKey, qint64 startEpoch)
{
    const quint8 streamByte = m_p.mainStream ? 0 : 1;
    const QByteArray streamName = m_p.mainStream ? QByteArrayLiteral("mainStream")
                                                 : QByteArrayLiteral("subStream");
    const QDateTime start = QDateTime::fromSecsSinceEpoch(startEpoch);
    const QDateTime dayEnd(start.date(), QTime(23, 59, 59));
    const QByteArray uid = m_p.uid.toUtf8();
    const QByteArray ch = QByteArray::number(m_p.channel);
    const QByteArray uidEl = uid.isEmpty() ? QByteArray() : ("<uid>" + uid + "</uid>\n");

    const QByteArray byTime =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n"
        "<logicChnBitmap>255</logicChnBitmap>\n<channelId>" + ch + "</channelId>\n" + uidEl +
        "<supportSub>" + QByteArray(m_p.mainStream ? "0" : "1") + "</supportSub>\n"
        "<streamType>" + streamName + "</streamType>\n"
        + bc::timeXml("startTime", start) + "\n"
        + bc::timeXml("endTime", dayEnd) + "\n</FileInfo>\n</FileInfoList>\n</body>\n";
    sock.write(frame(143, m_p.channel, streamByte, 20, 0, 0x6414, bc::aesCfb(byTime, aesKey, false)));
    sock.flush();
}

// Reverse play: fetch ~10 s windows of the stream behind the playhead (cmd 143, served in
// well under a second, several at once per recorder: ticket 12's probe), cut each at the
// playhead and send its keyframe groups newest first, every frame tagged with its recording
// time. The ring's backpressure holds the next window until the player needs it. Ends (the
// player sees the end of the stream) when a window has nothing before the playhead: the
// start of the recording; the page then jumps to the previous one or pauses.
void BaichuanClient::runReverse()
{
    BaichuanControl ctl({m_p.host, m_p.port, m_p.username, m_p.password});
    // A failure is an error for the player, not an end: an end means the recording's start.
    auto fail = [this] {
        QMutexLocker lock(&m_mutex);
        m_failed = true;
    };
    if (!ctl.open() || m_abort.load()) {
        qCWarning(lcCore).noquote() << "reverse playback: login failed on" << m_p.host;
        fail();
        return;
    }
    auto port = bcDownloadPort(ctl);
    qint64 cutoff = m_p.startEpoch * 1000;
    // Backwards stops at the start of the day being played (the page pauses there).
    const qint64 dayStartMs =
        QDateTime(QDateTime::fromSecsSinceEpoch(m_p.startEpoch).date(), QTime(0, 0)).toMSecsSinceEpoch();
    int lengthSecs = 10;
    while (!m_abort.load()) {
        const reverse::Window w = reverse::windowBefore(cutoff, lengthSecs);
        BcDownloadRequest req;
        req.channel = m_p.channel;
        req.uid = m_p.uid;
        req.subStream = !m_p.mainStream;
        req.start = QDateTime::fromSecsSinceEpoch(qMax(w.startSecs, dayStartMs / 1000));
        req.end = QDateTime::fromSecsSinceEpoch(w.endSecs);

        BcMediaParser parser;
        reverse::FrameClock clock;
        QList<reverse::Frame> frames;
        parser.onVideo = [&](const BcMediaParser::VideoFrame &f) {
            if (f.codec != BcMediaParser::Codec::Unknown) {
                QMutexLocker lock(&m_mutex);
                if (m_format.isEmpty())
                    m_format = f.codec == BcMediaParser::Codec::H265 ? QStringLiteral("hevc")
                                                                     : QStringLiteral("h264");
            }
            frames.append({f.keyFrame, clock.epochMs(f.keyFrame, f.time, f.microseconds), f.codec,
                           f.annexB});
        };
        BcDownloadOptions o;
        o.cancel = &m_abort;
        o.stallMs = 15000;
        const BcDownloadResult r =
            runBcDownload(*port, req, [&](const QByteArray &b) { parser.append(b); }, o);
        if (m_abort.load())
            return;
        if (!r.ok() && r.outcome != BcDownloadResult::Outcome::NoData) {
            qCWarning(lcCore).noquote() << "reverse playback: window ending"
                                        << req.end.toString(Qt::ISODate) << "failed, outcome"
                                        << int(r.outcome) << "status" << r.status;
            fail();
            return;
        }
        const reverse::Gops g = reverse::reverseGops(frames, cutoff, dayStartMs);
        if (g.nextCutoffMs < 0) {
            // Frames but no keyframe before the cutoff: a group longer than the window.
            // Fetch further back; nothing at all is the start of the recording (or the day).
            const bool someBefore = std::any_of(frames.begin(), frames.end(), [&](const reverse::Frame &f) {
                return f.epochMs >= dayStartMs && f.epochMs < cutoff;
            });
            if (someBefore && lengthSecs < 60 && w.startSecs * 1000 > dayStartMs) {
                lengthSecs = qMin(60, lengthSecs * 3);
                continue;
            }
            return;
        }
        lengthSecs = 10;
        for (const QList<reverse::Frame> &gop : g.newestFirst)
            for (const reverse::Frame &f : gop)
                pushAnnexB(frametime::tag(f.annexB, f.codec, f.epochMs));
        cutoff = g.nextCutoffMs;
    }
}

bool BaichuanClient::seek(qint64 startEpoch)
{
    if (startEpoch <= 0 || m_p.reverse)
        return false; // a reverse run reopens at the new moment
    QMutexLocker lock(&m_mutex);
    if (m_finished || m_abort.load())
        return false;
    m_seekEpoch = startEpoch;
    return true;
}

void BaichuanClient::setAudioHandler(std::function<void(const QByteArray &)> handler)
{
    m_audioHandler = std::move(handler);
}

void BaichuanClient::pumpMedia(QTcpSocket &sock, const QByteArray &aesKey, quint16 streamMsgNum)
{
    BcMediaParser parser;
    bool started = false;
    // Recorded playback drops video until the first I-frame, and is paced on video only:
    // audio ahead of that would play without a picture, or in a burst that overruns the
    // sound buffer. So hold it back until the picture starts.
    const bool holdAudio = m_p.startEpoch > 0;
    if (m_audioHandler) // empty = the parser skips audio
        parser.onAudio = [&, handler = m_audioHandler](const QByteArray &a) {
            if (!holdAudio || started)
                handler(a);
        };
    // After a seek, discard old in-flight frames until an I-frame at the new
    // position (POSIX seconds); 0 = no seek pending.
    qint64 seekTarget = 0;
    // Recorded playback is delivered faster than realtime, while selected live views
    // can arrive in short network bursts. Pace both from the camera clock; live uses a
    // small cushion to absorb bursts without discarding encoded reference frames.
    const bool pace = m_p.startEpoch > 0;
    const bool paceLive = !pace && m_p.smoothLive;
    QElapsedTimer clock;
    QElapsedTimer liveClock;
    if (paceLive)
        liveClock.start();
    BcLiveFramePacer livePacer;
    quint32 firstMicros = 0;
    double pacedSpeed = 1.0; // speed the pacing anchor was set at
    // Where playback is (for speed changes): only keyframes carry a usable wall-clock time
    // (other frames' `time` is junk), so it is the last keyframe's time plus the frame clock since.
    quint32 lastTime = 0;
    quint32 keyTime = 0, keyMicros = 0;
    // A restart waits for the recorder to answer the stop (382) before the new start (381):
    // sent back to back, the stop also ends the new replay. 0 = none pending.
    qint64 restartAt = 0;
    int restartSpeed = 1;
    QElapsedTimer restartWait;
    parser.onVideo = [&](const BcMediaParser::VideoFrame &f) {
        if (f.codec != BcMediaParser::Codec::Unknown) {
            QMutexLocker lock(&m_mutex);
            if (m_format.isEmpty())
                m_format = (f.codec == BcMediaParser::Codec::H265) ? QStringLiteral("hevc")
                                                                   : QStringLiteral("h264");
        }
        if (!started) {
            if (!f.keyFrame)
                return; // recorded playback opens mid-GOP; wait for the first I-frame
            // On a seek, the connection still carries old-position frames briefly;
            // drop them until an I-frame lands at the requested time.
            if (seekTarget > 0 && f.time != 0 && qAbs(qint64(f.time) - seekTarget) > 120)
                return;
            started = true;
            seekTarget = 0;
        }
        if (f.keyFrame && f.time != 0) {
            keyTime = f.time;
            keyMicros = f.microseconds;
        }
        if (keyTime != 0)
            lastTime = keyTime + quint32(qMax<qint64>(0, qint64(f.microseconds) - qint64(keyMicros)) / 1000000);
        if (pace && f.microseconds) {
            if (!clock.isValid()) {
                clock.start();
                firstMicros = f.microseconds;
            }
            const double speed = m_speed ? m_speed->load(std::memory_order_relaxed) : 1.0;
            if (speed != pacedSpeed) { // a speed change: re-anchor at this frame
                clock.restart();
                firstMicros = f.microseconds;
                pacedSpeed = speed;
            }
            const qint64 targetMs =
                qint64(double(qint64(f.microseconds) - qint64(firstMicros)) / 1000.0 / pacedSpeed);
            const qint64 elapsed = clock.elapsed();
            if (targetMs < 0 || targetMs - elapsed > 10000) {
                clock.restart(); // timestamp discontinuity/seek — rebase
                firstMicros = f.microseconds;
            } else {
                for (qint64 wait = targetMs - elapsed; wait > 0 && !m_abort.load();
                     wait = targetMs - clock.elapsed())
                    QThread::msleep(static_cast<unsigned long>(qMin<qint64>(wait, 50)));
            }
        }
        if (paceLive && (f.microseconds != 0 || livePacer.hasAnchor())) {
            livePacer.push(f.annexB, f.microseconds,
                           liveClock.nsecsElapsed() / 1000, f.keyFrame);
        } else if (pace && keyTime != 0) {
            // Recorded playback: tag the frame with its recording time for the playhead.
            const qint64 sinceKeyUs = qMax<qint64>(0, qint64(f.microseconds) - qint64(keyMicros));
            pushAnnexB(frametime::tag(f.annexB, f.codec, qint64(keyTime) * 1000 + sinceKeyUs / 1000));
        } else {
            pushAnnexB(f.annexB);
        }
    };

    auto releaseDueLiveFrame = [&]() {
        if (!paceLive)
            return false;
        QByteArray annexB;
        if (!livePacer.popDue(liveClock.nsecsElapsed() / 1000, annexB))
            return false;
        pushAnnexB(annexB);
        return true;
    };

    QByteArray netBuf = m_streamTail;
    Msg m;
    int stalls = 0;
    while (!m_abort.load()) {
        releaseDueLiveFrame();
        // In-place seek: reissue by-time playback and reset the media pipeline. The
        // BC message framing stays intact (continuous connection); only the BCMedia
        // layer resets, resyncing on the new position's first I-frame.
        qint64 seekTo = 0;
        {
            QMutexLocker lock(&m_mutex);
            if (m_seekEpoch > 0) {
                seekTo = m_seekEpoch;
                m_seekEpoch = 0;
            }
        }
        // A new recorder speed restarts the replay from the last frame shown (382 then 381);
        // ¼× and ½× stay at 1× on the recorder and only change the pacing below.
        if (seekTo <= 0 && pace && started && lastTime != 0 && wantedReplaySpeed() != m_replaySpeed)
            seekTo = lastTime;
        if (seekTo > 0) {
            const int speed = wantedReplaySpeed();
            if (restartAt == 0 && speed == m_replaySpeed
                && bc::seekInPlace(m_replayDay, QDateTime::fromSecsSinceEpoch(seekTo))) {
                sendReplaySeek(sock, aesKey, seekTo);
            } else {
                if (restartAt == 0) {
                    sendReplayStop(sock, aesKey);
                    restartWait.start(); // a later seek while waiting keeps this deadline
                }
                restartAt = seekTo;
                restartSpeed = speed;
                m_replaySpeed = speed; // so the speed check above doesn't fire again meanwhile
            }
            parser.reset();
            started = false;
            seekTarget = seekTo; // discard until an I-frame at the new position
            if (m_audioHandler)
                m_audioHandler(QByteArray()); // drop sound queued from the old position
            clock.invalidate();
            firstMicros = 0;
            keyTime = keyMicros = 0;
            QMutexLocker lock(&m_mutex);
            m_ring.clear(); // drop queued old-position frames so playback jumps
            m_cond.wakeAll();
        }
        // Start the new replay once the stop is answered (or after 2 s without an answer), with
        // a fresh parser: anything of the old replay still in flight was dropped meanwhile.
        auto startPendingReplay = [&] {
            parser.reset();
            sendReplayStart(sock, aesKey, restartAt, restartSpeed);
            restartAt = 0;
        };
        if (restartAt > 0 && restartWait.elapsed() > 2000)
            startPendingReplay();
        if (!parseMessage(netBuf, m)) {
            // Short wait so abort is honored quickly; recorded playback bursts then
            // stalls, so nudge it with a keep-alive after ~1s of silence.
            const qint64 dueUs = paceLive ? livePacer.nextDueUs() : -1;
            const qint64 waitForDueUs = dueUs < 0 ? -1 : dueUs - liveClock.nsecsElapsed() / 1000;
            const bool waitingForVideo = dueUs >= 0 && waitForDueUs < 300000;
            const int waitMs = waitingForVideo
                ? int(qMax<qint64>(1, (waitForDueUs + 999) / 1000))
                : 300;
            if (!sock.waitForReadyRead(waitMs)) {
                if (waitingForVideo) {
                    releaseDueLiveFrame();
                    continue;
                }
                if (++stalls >= 4) {
                    stalls = 0;
                    sock.write(frame(93, 0, 0, 99, 0, 0x6414, QByteArray())); // keep-alive: connection-level
                    sock.flush();
                }
                continue;
            }
            stalls = 0;
            netBuf.append(sock.readAll());
            continue;
        }
        if (restartAt > 0 && m.id == bc::kReplayStop)
            startPendingReplay();
        if (m.msgNum != streamMsgNum || restartAt > 0) // the old replay's frames: dropped
            continue;
        // FullAES: the Extension carries <encryptLen> — AES-decrypt that many bytes
        // of the payload, the remainder is plaintext.
        int encLen = 0;
        if (m.payloadOffset > 0) {
            const QByteArray ext = bc::aesCfb(m.body.left(m.payloadOffset), aesKey, true);
            const int p = ext.indexOf("<encryptLen>");
            if (p >= 0) {
                const int q = ext.indexOf("</encryptLen>", p);
                encLen = ext.mid(p + 12, q - p - 12).toInt();
            }
        }
        const QByteArray payload = m.body.mid(m.payloadOffset);
        const QByteArray media = encLen > 0
            ? bc::aesCfb(payload.left(encLen), aesKey, true) + payload.mid(encLen)
            : payload;
        parser.append(media);
    }
}

} // namespace rl
