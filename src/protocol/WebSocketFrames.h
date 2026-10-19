#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QtEndian>

// The client side of RFC 6455 that FrigateLive needs: the handshake key check, masked
// frames out, frames in (text, ping/pong, close, continuation), and the extended lengths.
// Kept here, rather than linking Qt WebSockets, so the app's bundled Qt needs no new module.
namespace rl::ws {

enum Opcode : quint8 { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

// Sec-WebSocket-Accept the server must answer for a Sec-WebSocket-Key.
inline QByteArray acceptFor(const QByteArray &key)
{
    return QCryptographicHash::hash(key + QByteArrayLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
                                    QCryptographicHash::Sha1)
        .toBase64();
}

// One final frame from the client; clients must mask, with any 4-byte key.
inline QByteArray clientFrame(quint8 opcode, const QByteArray &payload, quint32 maskKey)
{
    QByteArray f;
    f.append(char(0x80 | (opcode & 0x0F)));
    const qint64 n = payload.size();
    if (n < 126) {
        f.append(char(0x80 | n));
    } else if (n <= 0xFFFF) {
        f.append(char(0x80 | 126));
        char len[2];
        qToBigEndian<quint16>(quint16(n), len);
        f.append(len, 2);
    } else {
        f.append(char(0x80 | 127));
        char len[8];
        qToBigEndian<quint64>(quint64(n), len);
        f.append(len, 8);
    }
    char mask[4];
    qToBigEndian<quint32>(maskKey, mask);
    f.append(mask, 4);
    for (qint64 i = 0; i < n; ++i)
        f.append(char(payload[i] ^ mask[i % 4]));
    return f;
}

struct Frame {
    bool fin = true;
    quint8 opcode = 0;
    QByteArray payload; // unmasked
};

// Take one complete frame off the front of `buf`. False (buf untouched) if incomplete.
inline bool takeFrame(QByteArray &buf, Frame &out)
{
    if (buf.size() < 2)
        return false;
    const auto b0 = quint8(buf[0]);
    const auto b1 = quint8(buf[1]);
    const bool masked = b1 & 0x80;
    qint64 n = b1 & 0x7F;
    int at = 2;
    if (n == 126) {
        if (buf.size() < 4)
            return false;
        n = qFromBigEndian<quint16>(buf.constData() + 2);
        at = 4;
    } else if (n == 127) {
        if (buf.size() < 10)
            return false;
        n = qint64(qFromBigEndian<quint64>(buf.constData() + 2));
        at = 10;
    }
    const int maskAt = at;
    if (masked)
        at += 4;
    if (n < 0 || buf.size() < at + n)
        return false;
    out.fin = b0 & 0x80;
    out.opcode = b0 & 0x0F;
    out.payload = buf.mid(at, n);
    if (masked)
        for (qint64 i = 0; i < n; ++i)
            out.payload[i] = char(out.payload[i] ^ buf[maskAt + i % 4]);
    buf.remove(0, at + n);
    return true;
}

} // namespace rl::ws
