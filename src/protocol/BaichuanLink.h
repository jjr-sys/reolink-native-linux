#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace rl {

struct BaichuanParams
{
    QString host;
    int port = 9000;
    QString username;
    QString password;
};

// The seam for Baichuan (TCP 9000) config reads and writes: one logged-in session to
// one host. The real adapter is BaichuanControl; tests use a scripted adapter. Blocking,
// worker threads only. Streaming and push (readPushed, sendBinary, ...) are not part of
// this interface; their users keep the concrete class.
class BaichuanLink
{
public:
    virtual ~BaichuanLink() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // GET a command's config XML for a channel (channel < 0 = host level); empty on failure.
    virtual QByteArray get(quint32 cmdId, int channel, quint16 *statusOut = nullptr) = 0;
    // The recursive <enable> flag of a GET reply; -1 if absent or failed.
    virtual int readEnable(quint32 getCmdId, int channel) = 0;
    virtual bool writeEnable(quint32 getCmdId, quint32 setCmdId, int channel, bool enable) = 0;
    virtual QVariantMap getConfigFlat(quint32 cmdId, int channel,
                                      const QByteArray &reqBody = {}) = 0;
    virtual bool writeFields(quint32 getCmdId, quint32 setCmdId, int channel,
                             const QVariantMap &changes, const QByteArray &getBody = {}) = 0;
    virtual QByteArray transact(quint32 cmdId, int channel, const QByteArray &bodyXml,
                                quint16 *statusOut = nullptr) = 0;
};

// How a session to a host is made. Production: a BaichuanControl. Tests: a scripted link.
using BaichuanOpener = std::function<std::unique_ptr<BaichuanLink>(const BaichuanParams &)>;
BaichuanOpener realBaichuanOpener();

} // namespace rl
