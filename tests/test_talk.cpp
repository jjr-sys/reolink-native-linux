#include "media/TalkCodec.h"
#include "protocol/BaichuanTalk.h"
#include "protocol/BcCrypto.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace {

const char kAbilityXml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<TalkAbility version=\"1.1\">\n"
    "<duplexList><duplex>FDX</duplex></duplexList>\n"
    "<audioStreamModeList><audioStreamMode>followVideoStream</audioStreamMode></audioStreamModeList>\n"
    "<audioConfigList><audioConfig><priority>0</priority><audioType>adpcm</audioType>"
    "<sampleRate>16000</sampleRate><samplePrecision>16</samplePrecision>"
    "<lengthPerEncoder>1024</lengthPerEncoder><soundTrack>mono</soundTrack></audioConfig>"
    "</audioConfigList>\n</TalkAbility>\n</body>\n";

QByteArray sine(int samples, int rate, double hz, double amp = 12000)
{
    QByteArray pcm(samples * 2, 0);
    auto *p = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < samples; ++i)
        p[i] = static_cast<qint16>(amp * std::sin(2.0 * M_PI * hz * i / rate));
    return pcm;
}

void le32(QByteArray &b, quint32 v)
{
    char t[4];
    qToLittleEndian(v, t);
    b.append(t, 4);
}
void le16(QByteArray &b, quint16 v)
{
    char t[2];
    qToLittleEndian(v, t);
    b.append(t, 2);
}

// ---- A just-enough Baichuan device: login, TalkAbility, TalkConfig, Talk, TalkReset.
class MockCamera : public QObject
{
public:
    explicit MockCamera(bool busyFirst = false) : m_busyFirst(busyFirst)
    {
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            m_sock = m_server.nextPendingConnection();
            connect(m_sock, &QTcpSocket::readyRead, this, [this] { onData(); });
        });
    }
    quint16 port() const { return m_server.serverPort(); }

    QList<QByteArray> frames;     // cmd-202 payloads, in order
    QByteArray configBody;        // decrypted TalkConfig XML (last accepted)
    QByteArray binaryExt;         // decrypted Extension of the first cmd-202
    int resets = 0;
    int configAttempts = 0;
    QStringList seen;

private:
    struct In {
        quint32 cmd;
        quint8 ch;
        quint32 messId;
        quint32 offset;
        QByteArray body;
    };

    bool take(In &m)
    {
        if (m_buf.size() < 20)
            return false;
        const auto *d = reinterpret_cast<const uchar *>(m_buf.constData());
        const quint16 cls = qFromLittleEndian<quint16>(d + 18);
        const bool modern = cls == 0x6414 || cls == 0x0000;
        const int hdr = modern ? 24 : 20;
        const quint32 len = qFromLittleEndian<quint32>(d + 8);
        if (m_buf.size() < hdr + int(len))
            return false;
        m.cmd = qFromLittleEndian<quint32>(d + 4);
        m.ch = d[12];
        m.messId = d[13] | (d[14] << 8) | (d[15] << 16);
        m.offset = modern ? qFromLittleEndian<quint32>(d + 20) : 0;
        m.body = m_buf.mid(hdr, len);
        m_buf.remove(0, hdr + len);
        return true;
    }

    void send(quint32 cmd, quint8 ch, quint32 messId, quint16 status, bool modern,
              const QByteArray &body)
    {
        QByteArray m;
        le32(m, 0x0ABCDEF0);
        le32(m, cmd);
        le32(m, body.size());
        m.append(char(ch));
        m.append(char(messId & 0xff));
        m.append(char((messId >> 8) & 0xff));
        m.append(char((messId >> 16) & 0xff));
        le16(m, status);
        le16(m, modern ? 0x6414 : 0x6514);
        if (modern)
            le32(m, 0);
        m.append(body);
        m_sock->write(m);
    }

    void onData()
    {
        m_buf.append(m_sock->readAll());
        In m;
        while (take(m)) {
            const QByteArray key = rl::bc::aesKey(QStringLiteral("NONCE123"), QStringLiteral("pw"));
            if (m.cmd == 1 && m.body.isEmpty()) { // legacy hello -> nonce
                seen << "hello";
                send(1, 0, 0, 0, false,
                     rl::bc::xorCrypt("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
                                      "<Encryption version=\"1.1\">\n<type>md5</type>\n"
                                      "<nonce>NONCE123</nonce>\n</Encryption>\n</body>\n",
                                      0));
            } else if (m.cmd == 1) { // login
                seen << "login";
                send(1, 0, 0, 200, true, {});
            } else if (m.cmd == 10) {
                seen << "ability";
                send(10, m.ch, m.messId, 200, true,
                     rl::bc::aesCfb(QByteArray(kAbilityXml), key, false));
            } else if (m.cmd == 201) {
                seen << "config";
                ++configAttempts;
                const QByteArray xml = rl::bc::aesCfb(m.body.mid(m.offset), key, true);
                if (m_busyFirst && configAttempts == 1) {
                    send(201, m.ch, m.messId, 422, true, {});
                } else {
                    configBody = xml;
                    send(201, m.ch, m.messId, 200, true, {});
                }
            } else if (m.cmd == 202) {
                if (frames.isEmpty())
                    binaryExt = rl::bc::aesCfb(m.body.left(m.offset), key, true);
                frames << m.body.mid(m.offset); // payload is NOT ciphered
                send(202, m.ch, m.messId, 200, true, {});
            } else if (m.cmd == 11) {
                seen << "reset";
                ++resets;
                send(11, m.ch, m.messId, 200, true, {});
            }
        }
    }

    bool m_busyFirst;
    QTcpServer m_server;
    QTcpSocket *m_sock = nullptr;
    QByteArray m_buf;
};

} // namespace

class TestTalk : public QObject
{
    Q_OBJECT
private slots:
    // Our encoder's output must decode, with an independent decoder (libavcodec's
    // IMA WAV), back to the signal that went in.
    void adpcmDecodesInFfmpeg()
    {
        rl::ImaAdpcm enc(512); // 1025 samples per 516-byte block
        const int rate = 16000, blocks = 12;
        const QByteArray src = sine(enc.samplesPerBlock() * blocks, rate, 440);
        const auto *in = reinterpret_cast<const qint16 *>(src.constData());

        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ADPCM_IMA_WAV);
        QVERIFY(codec);
        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        ctx->sample_rate = rate;
        ctx->block_align = enc.blockBytes();
        ctx->bits_per_coded_sample = 4; // IMA ADPCM is 4 bits per sample
        av_channel_layout_default(&ctx->ch_layout, 1);
        QVERIFY(avcodec_open2(ctx, codec, nullptr) >= 0);
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        QList<qint16> decoded;
        for (int b = 0; b < blocks; ++b) {
            const QByteArray blk = enc.encodeBlock(in + b * enc.samplesPerBlock());
            QCOMPARE(blk.size(), enc.blockBytes());
            // Our own reference decoder must agree exactly with FFmpeg's.
            const QList<qint16> ref = rl::ImaAdpcm::decodeBlock(blk);
            QCOMPARE(ref.size(), enc.samplesPerBlock());

            av_new_packet(pkt, blk.size());
            memcpy(pkt->data, blk.constData(), blk.size());
            QCOMPARE(avcodec_send_packet(ctx, pkt), 0);
            av_packet_unref(pkt);
            QCOMPARE(avcodec_receive_frame(ctx, frame), 0);
            QCOMPARE(frame->nb_samples, enc.samplesPerBlock());
            const auto *f = reinterpret_cast<const qint16 *>(frame->data[0]);
            for (int i = 0; i < frame->nb_samples; ++i) {
                QCOMPARE(f[i], ref[i]);
                decoded.append(f[i]);
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);

        // ...and be a faithful copy of the input (IMA gives ~30 dB on a clean tone).
        double sig = 0, err = 0;
        for (int i = 0; i < decoded.size(); ++i) {
            sig += double(in[i]) * in[i];
            const double d = double(in[i]) - decoded[i];
            err += d * d;
        }
        const double snr = 10 * std::log10(sig / err);
        QVERIFY2(snr > 25, qPrintable(QString::number(snr)));
    }

    void encoderBuffersToWholeBlocks()
    {
        rl::TalkEncoder enc(512);
        QCOMPARE(enc.push(sine(1000, 16000, 300)).size(), 0);          // not a block yet
        QCOMPARE(enc.push(sine(1000, 16000, 300)).size(), 1);          // 2000 >= 1025
        QCOMPARE(enc.push(sine(4000, 16000, 300)).size(), 4 - 0);      // carries the remainder
        QCOMPARE(enc.blockMs(16000), 64);
    }

    void resamplesMicToMono()
    {
        // 48 kHz stereo float (a typical sound card) -> 16 kHz mono S16.
        rl::MicResampler rs(48000, 2, rl::MicSampleFormat::Float, 16000);
        QVERIFY(rs.isValid());
        QByteArray in(4800 * 2 * 4, 0);
        auto *f = reinterpret_cast<float *>(in.data());
        for (int i = 0; i < 4800; ++i)
            f[2 * i] = f[2 * i + 1] = 0.5f * std::sin(2 * M_PI * 440.0 * i / 48000);
        QByteArray total;
        total += rs.convert(in);
        total += rs.convert(in);
        const int samples = total.size() / 2;
        QVERIFY2(qAbs(samples - 3200) < 200, qPrintable(QString::number(samples)));
        // Signal survives at roughly half full-scale.
        const auto *p = reinterpret_cast<const qint16 *>(total.constData());
        int peak = 0;
        for (int i = 0; i < samples; ++i)
            peak = std::max(peak, int(std::abs(p[i])));
        QVERIFY2(peak > 12000 && peak < 20000, qPrintable(QString::number(peak)));
    }

    void abilityAndConfig()
    {
        const auto f = rl::talk::parseAbility(QByteArray(kAbilityXml));
        QVERIFY(f.has_value());
        QCOMPARE(f->sampleRate, 16000);
        QCOMPARE(f->lengthPerEncoder, 1024);
        QCOMPARE(f->dataBytes(), 512);
        QCOMPARE(f->duplex, QStringLiteral("FDX"));
        QCOMPARE(f->streamMode, QStringLiteral("followVideoStream"));

        const QByteArray cfg = rl::talk::configXml(3, *f);
        QVERIFY(cfg.contains("<TalkConfig version=\"1.1\">"));
        QVERIFY(cfg.contains("<channelId>3</channelId>"));
        QVERIFY(cfg.contains("<audioType>adpcm</audioType>"));
        QVERIFY(cfg.contains("<sampleRate>16000</sampleRate>"));
        QVERIFY(cfg.contains("<lengthPerEncoder>1024</lengthPerEncoder>"));
        QVERIFY(cfg.contains("<soundTrack>mono</soundTrack>"));

        // Nothing we can send: no ADPCM offered, or nonsense values.
        QVERIFY(!rl::talk::parseAbility(
                     "<body><TalkAbility><audioConfigList><audioConfig><audioType>g711"
                     "</audioType><sampleRate>8000</sampleRate><lengthPerEncoder>320"
                     "</lengthPerEncoder></audioConfig></audioConfigList></TalkAbility></body>")
                     .has_value());
        QVERIFY(!rl::talk::parseAbility(
                     "<body><TalkAbility><audioConfigList><audioConfig><audioType>adpcm"
                     "</audioType><sampleRate>0</sampleRate><lengthPerEncoder>0"
                     "</lengthPerEncoder></audioConfig></audioConfigList></TalkAbility></body>")
                     .has_value());
        QVERIFY(!rl::talk::parseAbility("not xml").has_value());
    }

    void adpcmFrameLayout()
    {
        const QByteArray block(516, 'x');
        const QByteArray f = rl::talk::frameAdpcm(block);
        QCOMPARE(f.left(4), QByteArray("01wb"));
        const auto *d = reinterpret_cast<const uchar *>(f.constData());
        QCOMPARE(qFromLittleEndian<quint16>(d + 4), quint16(520)); // block + 4
        QCOMPARE(qFromLittleEndian<quint16>(d + 6), quint16(520));
        QCOMPARE(qFromLittleEndian<quint16>(d + 8), quint16(0x0100));
        QCOMPARE(qFromLittleEndian<quint16>(d + 10), quint16(256)); // (516-4)/2
        QCOMPARE(f.mid(12, 516), block);
        // Trailing pad rounds the block (not the whole frame) up to 8 bytes.
        QCOMPARE(f.size(), 12 + 516 + (8 - 516 % 8) % 8);
    }

    // The whole session against a mock device: login, ability, config, data, reset.
    void sessionAgainstMockCamera()
    {
        MockCamera cam;
        rl::BaichuanTalk::Params p;
        p.host = QStringLiteral("127.0.0.1");
        p.port = cam.port();
        p.username = QStringLiteral("admin");
        p.password = QStringLiteral("pw");
        p.channel = 2;

        rl::TalkEncoder enc(512);
        const QList<QByteArray> blocks = enc.push(sine(1025 * 5, 16000, 500));
        QCOMPARE(blocks.size(), 5);

        bool opened = false, sentAll = true;
        QString err;
        std::thread t([&] {
            rl::BaichuanTalk talk(p);
            opened = talk.open(&err);
            if (opened) {
                for (const QByteArray &b : blocks)
                    sentAll = sentAll && talk.send(b, &err);
                talk.close();
            }
        });
        QElapsedTimer clock;
        clock.start();
        while (cam.resets == 0 && clock.elapsed() < 8000)
            QTest::qWait(20);
        t.join();

        QVERIFY2(opened, qPrintable(err));
        QVERIFY(sentAll);
        QCOMPARE(cam.seen, (QStringList{"hello", "login", "ability", "config", "reset"}));
        QVERIFY(cam.configBody.contains("<channelId>2</channelId>"));
        QVERIFY(cam.configBody.contains("<audioType>adpcm</audioType>"));
        QVERIFY(cam.binaryExt.contains("<binaryData>1</binaryData>"));
        QVERIFY(cam.binaryExt.contains("<channelId>2</channelId>"));
        QCOMPARE(cam.frames.size(), 5);
        for (int i = 0; i < 5; ++i)
            QCOMPARE(cam.frames[i], rl::talk::frameAdpcm(blocks[i]));
    }

    // A camera still holding an earlier session answers 422; the client resets it
    // and retries once, as the official client does.
    void busyCameraIsResetAndRetried()
    {
        MockCamera cam(/*busyFirst=*/true);
        rl::BaichuanTalk::Params p;
        p.host = QStringLiteral("127.0.0.1");
        p.port = cam.port();
        p.username = QStringLiteral("admin");
        p.password = QStringLiteral("pw");

        bool opened = false;
        QString err;
        std::thread t([&] {
            rl::BaichuanTalk talk(p);
            opened = talk.open(&err);
            talk.close();
        });
        QElapsedTimer clock;
        clock.start();
        while (cam.resets < 2 && clock.elapsed() < 8000)
            QTest::qWait(20); // one reset to clear the stale session, one on close
        t.join();

        QVERIFY2(opened, qPrintable(err));
        QCOMPARE(cam.configAttempts, 2);
    }
};

QTEST_MAIN(TestTalk)
#include "test_talk.moc"
