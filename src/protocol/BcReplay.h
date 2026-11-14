#pragma once

#include <QByteArray>
#include <QDateTime>

// Recorder-paced replay on Reolink NVRs (wayfinder/playback-controls ticket 10). cmd 381
// ReplayByTimeV2 starts a replay that the recorder paces at `playSpeed` (1-32; at 16 and above
// the sub stream sends I-frames only), 383 ReplaySeekV2 moves it in place, 382 (no body) stops
// it. All three go out in the playback client's framing (header channel = camera, msg_num 20,
// no Extension); with the control connection's framing the recorder answers 400. Media comes
// back on msg_num 20 as BCMedia, like cmd 143. Times are the recorder's local time.
namespace rl::bc {

inline constexpr quint32 kReplayStart = 381;
inline constexpr quint32 kReplayStop = 382;
inline constexpr quint32 kReplaySeek = 383;

// `<tag><year>…</year>…<second>…</second></tag>`, the time element every replay and download
// request uses (recorder local time).
QByteArray timeXml(const char *tag, const QDateTime &t);

// The speed to ask the recorder for: whole speeds 1-32. Below 1x it plays at 1x and the app
// slows it.
int recorderSpeed(double speed);

// cmd 381 body: play `channel` from `start` to the end of that day at `playSpeed`.
QByteArray replayStartXml(int channel, bool mainStream, int playSpeed, const QDateTime &start);
// cmd 383 body: move the running replay to `to` (the recorder resumes at the keyframe before).
QByteArray replaySeekXml(int channel, bool mainStream, const QDateTime &to);
// Whether a seek in a replay of `replayDay` to `to` can move in place (383): the same day,
// earlier or later than where the replay started (383 carries its own window; checked on the
// RLN8 30 min back). Another day stops the replay and starts it again.
bool seekInPlace(const QDate &replayDay, const QDateTime &to);

} // namespace rl::bc
