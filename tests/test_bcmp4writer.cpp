#include "media/BcMediaParser.h"
#include "media/BcMp4Writer.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace rl;
using Codec = BcMediaParser::Codec;

namespace {

// An ADTS header: AAC LC, sample-rate index `sr`, `ch` channels, `len` bytes in all.
QByteArray adts(int sr, int ch, int len, bool crc = false)
{
    QByteArray h(crc ? 9 : 7, '\0');
    const int profile = 1; // object type 2 - 1
    h[0] = char(0xFF);
    h[1] = char(crc ? 0xF0 : 0xF1);
    h[2] = char((profile << 6) | (sr << 2) | (ch >> 2));
    h[3] = char(((ch & 3) << 6) | ((len >> 11) & 3));
    h[4] = char((len >> 3) & 0xFF);
    h[5] = char(((len & 7) << 5) | 0x1F);
    h[6] = char(0xFC);
    return h + QByteArray(len - h.size(), 'a');
}

QByteArray nal(QByteArray head, QByteArray rest = "xyz")
{
    return QByteArray("\x00\x00\x00\x01", 4) + head + rest;
}

} // namespace

class TestBcMp4Writer : public QObject
{
    Q_OBJECT
private slots:
    void parseAdts_readsTheHeader()
    {
        const auto h = parseAdts(adts(8, 1, 200));
        QVERIFY(h);
        QCOMPARE(h->headerLen, 7);
        QCOMPARE(h->objectType, 2);
        QCOMPARE(h->srIndex, 8);
        QCOMPARE(h->sampleRate, 16000);
        QCOMPARE(h->channels, 1);
        QCOMPARE(h->frameLen, 200);
        QCOMPARE(parseAdts(adts(3, 2, 300, true))->headerLen, 9);
    }
    void parseAdts_rejectsJunk()
    {
        QVERIFY(!parseAdts("abc"));
        QVERIFY(!parseAdts(QByteArray(16, '\0')));
    }
    void audioSpecificConfig_isTwoBytes()
    {
        // object type 2, index 8 (16 kHz), mono: 00010 1000 0001 000
        QCOMPARE(aacAudioSpecificConfig(*parseAdts(adts(8, 1, 100))), QByteArray("\x14\x08", 2));
        // index 3 (48 kHz), stereo: 00010 0011 0010 000
        QCOMPARE(aacAudioSpecificConfig(*parseAdts(adts(3, 2, 100))), QByteArray("\x11\x90", 2));
    }
    void parameterSets_h264()
    {
        const QByteArray au = nal("\x67", "sps") + nal("\x68", "pps") + nal("\x06", "sei")
                              + nal("\x65", "idr");
        QCOMPARE(annexBParameterSets(au, Codec::H264), nal("\x67", "sps") + nal("\x68", "pps"));
    }
    void parameterSets_hevc()
    {
        const QByteArray au = nal(QByteArray("\x40\x01", 2), "vps") + nal(QByteArray("\x42\x01", 2), "sps")
                              + nal(QByteArray("\x44\x01", 2), "pps") + nal(QByteArray("\x26\x01", 2), "idr");
        QCOMPARE(annexBParameterSets(au, Codec::H265),
                 nal(QByteArray("\x40\x01", 2), "vps") + nal(QByteArray("\x42\x01", 2), "sps")
                     + nal(QByteArray("\x44\x01", 2), "pps"));
    }
    void parameterSets_threeByteStartCodes()
    {
        const QByteArray au = QByteArray("\x00\x00\x01\x67s", 5) + QByteArray("\x00\x00\x01\x65i", 5);
        QCOMPARE(annexBParameterSets(au, Codec::H264), nal("\x67", "s"));
    }
    void microsClock_unwraps()
    {
        MicrosClock c;
        QCOMPARE(c.push(0xFFFFFF00u), qint64(0));
        QCOMPARE(c.push(0xFFFFFFF0u), qint64(0xF0));
        QCOMPARE(c.push(0x10u), qint64(0x110));
        QCOMPARE(c.push(0x20u), qint64(0x120));
    }
    void writer_withoutVideoFailsAndLeavesNoFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("none.mp4");
        {
            BcMp4Writer w(path);
            QString err;
            QVERIFY(!w.finish(&err));
            QVERIFY(!err.isEmpty());
        }
        QVERIFY(!QFile::exists(path));
    }

    // Needs a captured Baichuan media stream (decrypted, as bcdlprobe saves it): set
    // RL_TEST_BCM to the .bcm file and RL_TEST_BCM_SECS to its length. Skipped when unset.
    void writer_realStreamGivesPlayableMp4()
    {
        const QString src = qEnvironmentVariable("RL_TEST_BCM");
        if (src.isEmpty())
            QSKIP("RL_TEST_BCM not set");
        const double secs = qEnvironmentVariable("RL_TEST_BCM_SECS", "15").toDouble();
        QFile in(src);
        QVERIFY(in.open(QIODevice::ReadOnly));
        QTemporaryDir dir;
        const QString path = dir.filePath("clip.mp4");
        BcMp4Writer w(path);
        BcMediaParser parser;
        parser.onVideo = [&](const BcMediaParser::VideoFrame &f) {
            w.addVideo(f, parser.declaredWidth(), parser.declaredHeight());
        };
        parser.onAudio = [&](const QByteArray &a) { w.addAudio(a); };
        while (!in.atEnd())
            parser.append(in.read(64 * 1024)); // chunked: frames cross the reads
        QString err;
        QVERIFY2(w.finish(&err), qPrintable(err));

        AVFormatContext *fmt = nullptr;
        QCOMPARE(avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr), 0);
        QVERIFY(avformat_find_stream_info(fmt, nullptr) >= 0);
        int video = -1, audio = -1;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVCodecParameters *p = fmt->streams[i]->codecpar;
            if (p->codec_type == AVMEDIA_TYPE_VIDEO) video = int(i);
            if (p->codec_type == AVMEDIA_TYPE_AUDIO) audio = int(i);
        }
        QVERIFY(video >= 0);
        QVERIFY(audio >= 0);
        QVERIFY(fmt->streams[video]->codecpar->width > 0);
        const double dur = double(fmt->duration) / AV_TIME_BASE;
        qInfo() << "duration" << dur << "s, frames" << w.videoFrames() << "audio" << w.audioFrames();
        QVERIFY2(qAbs(dur - secs) < 1.5, qPrintable(QString::number(dur)));

        // Every packet decodes: the video from its first frame, the audio cleanly.
        AVCodecContext *dec[2] = {};
        const int idx[2] = {video, audio};
        for (int k = 0; k < 2; ++k) {
            const AVCodecParameters *p = fmt->streams[idx[k]]->codecpar;
            dec[k] = avcodec_alloc_context3(avcodec_find_decoder(p->codec_id));
            avcodec_parameters_to_context(dec[k], p);
            QCOMPARE(avcodec_open2(dec[k], nullptr, nullptr), 0);
        }
        int decoded[2] = {}, errors = 0, packets = 0;
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        while (av_read_frame(fmt, pkt) >= 0) {
            const int k = pkt->stream_index == video ? 0 : pkt->stream_index == audio ? 1 : -1;
            if (k >= 0 && avcodec_send_packet(dec[k], pkt) >= 0)
                for (int r; (r = avcodec_receive_frame(dec[k], frame)) != AVERROR(EAGAIN);) {
                    if (r == AVERROR_EOF) break;
                    if (r < 0) { ++errors; break; }
                    ++decoded[k];
                }
            else if (k >= 0)
                ++errors;
            if (k == 0) ++packets;
            av_packet_unref(pkt);
        }
        for (int k = 0; k < 2; ++k) {
            avcodec_send_packet(dec[k], nullptr);
            while (avcodec_receive_frame(dec[k], frame) >= 0)
                ++decoded[k];
            avcodec_free_context(&dec[k]);
        }
        av_frame_free(&frame);
        av_packet_free(&pkt);
        qInfo() << "video packets" << packets << "decoded video" << decoded[0] << "audio" << decoded[1] << "errors" << errors;
        QCOMPARE(errors, 0);
        // The decoder can hold back a damaged last access unit; allow one frame.
        QVERIFY2(decoded[0] >= w.videoFrames() - 1, qPrintable(QString::number(decoded[0])));
        QCOMPARE(decoded[1], w.audioFrames());
        avformat_close_input(&fmt);
        if (qEnvironmentVariableIsSet("RL_TEST_KEEP"))
            QFile::copy(path, qEnvironmentVariable("RL_TEST_KEEP"));
    }
};

QTEST_GUILESS_MAIN(TestBcMp4Writer)
#include "test_bcmp4writer.moc"
