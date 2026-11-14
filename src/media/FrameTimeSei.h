#pragma once

#include "media/BcMediaParser.h"

#include <QByteArray>
#include <QtGlobal>

// The recording time of each Playback frame (wayfinder/playback-controls ticket 11).
//
// The recorder's frames reach the decoder as a bare Annex-B byte stream, which has no room
// for their recording time. So the recorder client tags each access unit with a
// user_data_unregistered SEI (payload type 5: our UUID + the time in milliseconds since the
// epoch, big-endian), placed before the first picture slice. FFmpeg's H.264/H.265 decoders
// hand that SEI back on the decoded frame (AV_FRAME_DATA_SEI_UNREGISTERED), so the player
// knows the recording time of the frame it shows, past every buffer on the way.
namespace rl::frametime {

// `annexB` with the time SEI inserted; unchanged for an unknown codec.
QByteArray tag(const QByteArray &annexB, BcMediaParser::Codec codec, qint64 epochMs);

// The SEI payload as a decoder hands it over (UUID + 8 bytes, emulation removed).
QByteArray userData(qint64 epochMs);

// The time from a decoder's user-data payload; -1 when it isn't ours.
qint64 fromUserData(const uchar *data, int size);

// The time in a tagged access unit; -1 when it has none.
qint64 find(const QByteArray &annexB, BcMediaParser::Codec codec);

} // namespace rl::frametime
