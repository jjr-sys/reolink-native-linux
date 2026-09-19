#include "BaichuanTalk.h"

#include "core/Log.h"

#include <QMap>
#include <QXmlStreamReader>
#include <QtEndian>

namespace rl {

namespace talk {

namespace {

void putLE16(QByteArray &b, quint16 v)
{
    char t[2];
    qToLittleEndian(v, t);
    b.append(t, 2);
}

} // namespace

std::optional<TalkFormat> parseAbility(const QByteArray &abilityXml)
{
    QStringList duplexes, modes;
    QList<QMap<QString, QString>> configs;

    QXmlStreamReader xml(abilityXml);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        const QString name = xml.name().toString();
        if (name == QLatin1String("duplex")) {
            duplexes << xml.readElementText().trimmed();
        } else if (name == QLatin1String("audioStreamMode")) {
            modes << xml.readElementText().trimmed();
        } else if (name == QLatin1String("audioConfig")) {
            QMap<QString, QString> cfg;
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isEndElement() && xml.name() == QLatin1String("audioConfig"))
                    break;
                if (xml.isStartElement()) {
                    const QString key = xml.name().toString();
                    cfg.insert(key, xml.readElementText().trimmed());
                }
            }
            configs << cfg;
        }
    }

    // ADPCM only; prefer mono among those (the encoder here is mono).
    const QMap<QString, QString> *pick = nullptr;
    for (const auto &c : std::as_const(configs)) {
        if (c.value(QStringLiteral("audioType")).compare(QLatin1String("adpcm"),
                                                         Qt::CaseInsensitive) != 0)
            continue;
        if (!pick || c.value(QStringLiteral("soundTrack")) == QLatin1String("mono"))
            pick = &c;
        if (c.value(QStringLiteral("soundTrack")) == QLatin1String("mono"))
            break;
    }
    if (!pick)
        return std::nullopt;

    TalkFormat f;
    f.audioType = QStringLiteral("adpcm");
    f.sampleRate = pick->value(QStringLiteral("sampleRate")).toInt();
    f.samplePrecision = pick->value(QStringLiteral("samplePrecision"), QStringLiteral("16")).toInt();
    f.lengthPerEncoder = pick->value(QStringLiteral("lengthPerEncoder")).toInt();
    f.soundTrack = pick->value(QStringLiteral("soundTrack"), QStringLiteral("mono"));
    if (f.sampleRate <= 0 || f.lengthPerEncoder < 16 || f.lengthPerEncoder % 2 != 0 ||
        f.lengthPerEncoder > 8192)
        return std::nullopt; // an ability we cannot trust is worse than none
    if (!duplexes.isEmpty())
        f.duplex = duplexes.contains(QStringLiteral("FDX")) ? QStringLiteral("FDX") : duplexes.first();
    if (!modes.isEmpty())
        f.streamMode = modes.contains(QStringLiteral("followVideoStream"))
                           ? QStringLiteral("followVideoStream")
                           : modes.first();
    return f;
}

QByteArray configXml(int channel, const TalkFormat &f)
{
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
                          "<TalkConfig version=\"1.1\">\n"
                          "<channelId>%1</channelId>\n"
                          "<duplex>%2</duplex>\n"
                          "<audioStreamMode>%3</audioStreamMode>\n"
                          "<audioConfig>\n"
                          "<audioType>%4</audioType>\n"
                          "<sampleRate>%5</sampleRate>\n"
                          "<samplePrecision>%6</samplePrecision>\n"
                          "<lengthPerEncoder>%7</lengthPerEncoder>\n"
                          "<soundTrack>%8</soundTrack>\n"
                          "</audioConfig>\n</TalkConfig>\n</body>\n")
        .arg(channel)
        .arg(f.duplex, f.streamMode, f.audioType)
        .arg(f.sampleRate)
        .arg(f.samplePrecision)
        .arg(f.lengthPerEncoder)
        .arg(f.soundTrack)
        .toUtf8();
}

QByteArray frameAdpcm(const QByteArray &block)
{
    QByteArray f;
    f.append("01wb", 4);                                    // BCMedia ADPCM audio
    putLE16(f, static_cast<quint16>(block.size() + 4));     // payload size (incl. sub-header)
    putLE16(f, static_cast<quint16>(block.size() + 4));     // ... repeated
    putLE16(f, 0x0100);                                     // ADPCM data marker
    putLE16(f, static_cast<quint16>((block.size() - 4) / 2)); // half the block's data bytes
    f.append(block);
    // Trailing pad rounds the *block* length up to 8 (not the frame): that is what
    // cameras have been seen to accept for talk audio.
    f.append(QByteArray((8 - block.size() % 8) % 8, 0));
    return f;
}

} // namespace talk

// ---- BaichuanTalk -----------------------------------------------------------

BaichuanTalk::BaichuanTalk(Params params) : m_p(std::move(params)) {}

BaichuanTalk::~BaichuanTalk()
{
    close();
}

bool BaichuanTalk::open(QString *error)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        qCWarning(lcProto) << "talk:" << m_p.host << "ch" << m_p.channel << why;
        close();
        return false;
    };

    BaichuanControl::Params cp;
    cp.host = m_p.host;
    cp.port = m_p.port;
    cp.username = m_p.username;
    cp.password = m_p.password;
    m_ctl = std::make_unique<BaichuanControl>(cp);
    if (!m_ctl->open())
        return fail(QObject::tr("Could not connect to the device (unreachable, wrong login, or "
                                "it has no free sessions)"));

    quint16 status = 0;
    const QByteArray ability = m_ctl->get(talk::kCmdAbility, m_p.channel, &status);
    if (ability.isEmpty() || (status != 200 && status != 0))
        return fail(QObject::tr("This camera did not report two-way audio support"));
    const auto chosen = talk::parseAbility(ability);
    if (!chosen)
        return fail(QObject::tr("This camera offers no audio format we can send"));
    m_format = *chosen;

    const QByteArray cfg = talk::configXml(m_p.channel, m_format);
    m_ctl->transact(talk::kCmdConfig, m_p.channel, cfg, &status);
    if (status == 422) {
        // Another talk session is live, or one was left open: end it and retry
        // once, which is what the official client does.
        m_ctl->transact(talk::kCmdReset, m_p.channel, QByteArray());
        m_ctl->transact(talk::kCmdConfig, m_p.channel, cfg, &status);
    }
    if (status != 200)
        return fail(status == 422
                        ? QObject::tr("The camera is busy with another talk session")
                        : QObject::tr("The camera refused the audio format (status %1)").arg(status));

    m_dataMessId = m_ctl->nextMessId();
    m_talking = true;
    qCInfo(lcProto) << "talk:" << m_p.host << "ch" << m_p.channel << "ready:" << m_format.sampleRate
                    << "Hz, block" << m_format.lengthPerEncoder;
    return true;
}

bool BaichuanTalk::send(const QByteArray &adpcmBlock, QString *error)
{
    if (!m_talking || !m_ctl)
        return false;
    if (!m_ctl->sendBinary(talk::kCmdData, m_p.channel, m_dataMessId, talk::frameAdpcm(adpcmBlock))) {
        if (error)
            *error = QObject::tr("Connection to the camera was lost");
        m_talking = false;
        return false;
    }
    const quint16 status = m_ctl->drainReplies();
    if (status >= 400) {
        if (error)
            *error = QObject::tr("The camera stopped accepting audio (status %1)").arg(status);
        m_talking = false;
        return false;
    }
    return true;
}

void BaichuanTalk::close()
{
    if (m_ctl && m_talking)
        m_ctl->transact(talk::kCmdReset, m_p.channel, QByteArray());
    m_talking = false;
    if (m_ctl)
        m_ctl->close();
    m_ctl.reset();
}

} // namespace rl
