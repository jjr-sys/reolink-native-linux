#include "media/FrameTimeSei.h"

#include <QList>

#include <cstring>

namespace rl::frametime {

namespace {

using Codec = BcMediaParser::Codec;

// Random UUID naming this app's frame-time SEI.
constexpr uchar kUuid[16] = {0x5e, 0x0c, 0x3a, 0x71, 0x9b, 0x24, 0x4f, 0x6d,
                             0xa1, 0x8e, 0x27, 0xc4, 0x90, 0x5b, 0xd3, 0x16};
constexpr int kPayload = 16 + 8;

bool isVcl(quint8 header, Codec codec)
{
    if (codec == Codec::H265)
        return ((header >> 1) & 0x3F) < 32;
    const int t = header & 0x1F;
    return t >= 1 && t <= 5;
}

// Escape start-code emulation: 00 00 followed by 00–03 gets an 03 in between.
QByteArray escape(const QByteArray &rbsp)
{
    QByteArray out;
    out.reserve(rbsp.size() + 4);
    int zeros = 0;
    for (const char c : rbsp) {
        const quint8 b = quint8(c);
        if (zeros >= 2 && b <= 3) {
            out.append(char(3));
            zeros = 0;
        }
        out.append(c);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

QByteArray unescape(const QByteArray &ebsp)
{
    QByteArray out;
    out.reserve(ebsp.size());
    int zeros = 0;
    for (const char c : ebsp) {
        const quint8 b = quint8(c);
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        out.append(c);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

QByteArray seiNal(Codec codec, qint64 epochMs)
{
    QByteArray rbsp;
    if (codec == Codec::H265)
        rbsp.append(char(39 << 1)).append(char(1)); // PREFIX_SEI, layer 0, temporal id 0 (+1)
    else
        rbsp.append(char(6)); // SEI, nal_ref_idc 0
    rbsp.append(char(5));        // user_data_unregistered
    rbsp.append(char(kPayload)); // payload size
    rbsp.append(userData(epochMs));
    rbsp.append(char(0x80));     // rbsp trailing bits
    return QByteArray("\x00\x00\x00\x01", 4) + escape(rbsp);
}

// Offset of each start code (00 00 01) and of the NAL header after it.
struct Nal {
    int start;  // where the start code begins (including a leading 00 of a 4-byte code)
    int header; // first byte after 00 00 01
};

QList<Nal> nals(const QByteArray &au)
{
    QList<Nal> out;
    for (int i = 0; i + 3 < au.size(); ++i) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const int start = (i > 0 && au[i - 1] == 0) ? i - 1 : i;
            out.append({start, i + 3});
            i += 2;
        }
    }
    return out;
}

} // namespace

QByteArray userData(qint64 epochMs)
{
    QByteArray out(reinterpret_cast<const char *>(kUuid), 16);
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(char((quint64(epochMs) >> shift) & 0xFF));
    return out;
}

qint64 fromUserData(const uchar *data, int size)
{
    if (!data || size < kPayload || memcmp(data, kUuid, 16) != 0)
        return -1;
    quint64 v = 0;
    for (int i = 16; i < kPayload; ++i)
        v = (v << 8) | data[i];
    return qint64(v);
}

QByteArray tag(const QByteArray &annexB, Codec codec, qint64 epochMs)
{
    if (codec == Codec::Unknown)
        return annexB;
    for (const Nal &n : nals(annexB)) {
        if (isVcl(quint8(annexB[n.header]), codec)) {
            QByteArray out = annexB.left(n.start);
            out.append(seiNal(codec, epochMs));
            out.append(annexB.mid(n.start));
            return out;
        }
    }
    return annexB + seiNal(codec, epochMs);
}

qint64 find(const QByteArray &annexB, Codec codec)
{
    const QList<Nal> list = nals(annexB);
    for (int i = 0; i < list.size(); ++i) {
        const int h = list[i].header;
        const quint8 b = quint8(annexB[h]);
        const bool sei = codec == Codec::H265 ? ((b >> 1) & 0x3F) == 39 : (b & 0x1F) == 6;
        if (!sei)
            continue;
        const int body = h + (codec == Codec::H265 ? 2 : 1);
        const int end = i + 1 < list.size() ? list[i + 1].start : annexB.size();
        const QByteArray rbsp = unescape(annexB.mid(body, end - body));
        if (rbsp.size() < 2 + kPayload || quint8(rbsp[0]) != 5 || quint8(rbsp[1]) != kPayload)
            continue;
        const qint64 t = fromUserData(reinterpret_cast<const uchar *>(rbsp.constData()) + 2, kPayload);
        if (t >= 0)
            return t;
    }
    return -1;
}

} // namespace rl::frametime
