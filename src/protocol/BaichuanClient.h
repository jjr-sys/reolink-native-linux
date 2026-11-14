#pragma once

#include "media/BcMediaParser.h"
#include "media/ReversePlay.h"

#include <QByteArray>
#include <QDate>
#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

class QTcpSocket;

// Native Reolink "Baichuan" (BC, TCP 9000) client — the transport the official
// apps use, and the only one that streams the HEVC main stream in real time for
// both live and recorded playback (HTTP-FLV is H.264-only on NVR firmware and the
// cmd=Download endpoint is throttled below realtime).
//
// Runs its network loop on a detached worker thread and exposes a blocking read()
// that yields a raw H.264/H.265 Annex-B elementary stream, designed to back a
// libavformat AVIOContext so StreamPlayer's existing decode path is reused as-is.
//
// Clean-room: framing/crypto from the documented wire format + MIT reolink_aio /
// reolink-aio-ts; the recorded-playback command flow (search cmd 14 -> list cmd 15
// -> replay cmd 5, with cmd 93 keep-alive) reverse-engineered against an RLN8-410.
// No AGPL neolink code.
namespace rl {

class BcDownloadPort;

class BaichuanClient
{
public:
    struct Params {
        QString host;
        int port = 9000;
        QString username;
        QString password;
        int channel = 0;
        QString uid;                    // camera UID (from GetChannelstatus)
        bool mainStream = true;
        bool smoothLive = false;        // pace live access units on their camera timestamps
        qint64 startEpoch = 0;          // 0 => live preview; else recorded playback from here
        // Recorded playback backwards from startEpoch (wayfinder/playback-controls ticket 12):
        // windows fetched with cmd 143, keyframe groups sent newest first (media/ReversePlay.h).
        bool reverse = false;
        // HD fast forward (wayfinder/live-replay ticket 09): windows ahead of startEpoch,
        // fetched with cmd 143, whole keyframe groups sent in order; the player paces them.
        bool forwardWindows = false;
    };

    explicit BaichuanClient(Params params);
    ~BaichuanClient();

    // Spawn the worker (connect + login + start the stream). Non-blocking.
    void start();
    // Signal the worker to stop and unblock any pending read(). Never joins on the
    // caller thread.
    void stop();

    // Seek an active recorded-playback session to a new instant, in place (reissues
    // the by-time command on the same connection — no reconnect/re-login). Returns
    // false if the session has already ended (caller should start a fresh one).
    bool seek(qint64 startEpoch);

    // Where the stream's AAC audio goes (one ADTS frame per call, on the client's
    // network thread). Set before start(); the raw video byte stream has no room for
    // it. Unset = audio is discarded.
    void setAudioHandler(std::function<void(const QByteArray &)> handler);
    // Playback speed shared with the player (1 = real time). Read as frames are paced.
    void setSpeedCell(std::shared_ptr<std::atomic<double>> cell) { m_speed = std::move(cell); }
    // Windows of the main stream (reverse play, HD fast forward): called once (network thread)
    // when the link can't feed them (media/ReversePlay.h LinkPace), or forwards when the
    // windows run out (the newest footage, a gap, an error): play SD from there. Set before
    // start().
    void setLinkSlowHandler(std::function<void()> handler) { m_linkSlow = std::move(handler); }

    // Blocking read of decoded Annex-B bytes (for an AVIOContext read callback).
    // Returns the number of bytes written, 0 on clean end, or <0 on abort/error.
    int read(unsigned char *buf, int size);

    // "h264" / "hevc" once known (after the first frame); empty until then.
    QString ffmpegFormat();

private:
    void run();                                    // worker entry
    void runReverse();                             // reverse play: windows behind the playhead
    void runForwardWindows();                      // HD fast forward: windows ahead of it
    struct FetchedWindow {
        QList<reverse::Frame> frames;
        double fetchSecs = 0;
        bool failed = false;   // refused or broken off (no recording there is not a failure)
    };
    FetchedWindow fetchWindow(BcDownloadPort &port, qint64 startSecs, qint64 endSecs);
    void pushFrames(const QList<reverse::Frame> &gop, bool keysOnly);
    // Backwards at this speed sends keyframes only, as the recorder's 16× does forwards.
    static constexpr double kKeyframesOnlySpeed = 16.0;
    bool connectAndLogin(QTcpSocket &sock, QByteArray &aesKey);
    bool startStream(QTcpSocket &sock, const QByteArray &aesKey);
    // Recorder-paced replay (cmd 381/382/383, wayfinder/playback-controls ticket 10).
    void sendReplayStart(QTcpSocket &sock, const QByteArray &aesKey, qint64 startEpoch, int speed);
    void sendReplaySeek(QTcpSocket &sock, const QByteArray &aesKey, qint64 toEpoch);
    void sendReplayStop(QTcpSocket &sock, const QByteArray &aesKey);
    void sendOnStream(QTcpSocket &sock, const QByteArray &aesKey, quint32 cmd, const QByteArray &xml);
    // cmd 143 download of the day from `startEpoch`: unpaced, used before 381; kept for the
    // reverse-play windows (wayfinder/playback-controls ticket 12).
    void sendDownloadByTime(QTcpSocket &sock, const QByteArray &aesKey, qint64 startEpoch);
    int wantedReplaySpeed() const;
    void pumpMedia(QTcpSocket &sock, const QByteArray &aesKey, quint16 streamMsgNum);
    void pushAnnexB(const QByteArray &bytes);

    Params m_p;
    std::thread m_thread;
    std::atomic<bool> m_abort{false};

    std::function<void(const QByteArray &)> m_audioHandler; // set before start()
    std::shared_ptr<std::atomic<double>> m_speed;           // set before start(); may be empty
    std::function<void()> m_linkSlow;                       // set before start(); may be empty

    QMutex m_mutex;
    QWaitCondition m_cond;
    QByteArray m_ring;        // decoded Annex-B awaiting the reader
    bool m_finished = false;  // worker done producing
    bool m_failed = false;    // ...because of an error (reverse play): read() reports it
    qint64 m_seekEpoch = 0;   // pending in-place seek target (0 = none)
    // The running replay (network thread only): the day it was started on and its speed.
    QDate m_replayDay;
    int m_replaySpeed = 1;
    QString m_format;         // "h264"/"hevc", guarded by m_mutex
    bool m_started = false;
    QByteArray m_loginTail;   // socket bytes buffered past the login reply
    QByteArray m_streamTail;  // socket bytes buffered past the stream-start reply
};

} // namespace rl
