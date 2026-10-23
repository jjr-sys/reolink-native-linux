#pragma once

// Scripted adapters for the two protocol seams: an HTTP-CGI transport that answers from
// a table and a Baichuan link that serves canned XML and records what is written.
// No sockets, no timers, no keyring.

#include "protocol/BaichuanLink.h"
#include "protocol/ReolinkTransport.h"

#include <QHash>
#include <QStringList>
#include <QVector>

namespace rl::testing {

class ScriptedHttp : public ReolinkTransport
{
public:
    // cmd name -> the value it answers with ("code": 0). A command not listed fails
    // with `rspCode` -9 (not supported).
    QHash<QString, Json> values;
    bool transportOk = true;
    QString transportError;
    FailKind failKind = FailKind::None;
    QStringList sent;      // every command name, in the order it was sent
    QVector<Json> sentParams;

    api::BatchResult call(const Json &commands) override
    {
        api::BatchResult b;
        b.transportOk = transportOk;
        b.error = transportError;
        if (!transportOk)
            return b;
        for (const Json &c : commands) {
            const QString name = QString::fromStdString(c.value("cmd", std::string{}));
            sent.append(name);
            sentParams.append(c.contains("param") ? c["param"] : Json::object());
            api::CommandResult r;
            r.cmd = name;
            if (values.contains(name)) {
                r.ok = true;
                r.value = values.value(name);
            } else {
                r.rspCode = -9;
                r.detail = QStringLiteral("not supported");
            }
            b.results.append(r);
        }
        return b;
    }
    FailKind lastFailKind() override { return failKind; }
};

// One shared script behind every session an opener hands out, so a test sees the sum of
// what the code under test did across its short sessions.
struct BaichuanScript
{
    bool canOpen = true;
    QHash<quint32, QByteArray> replies;       // cmd id -> XML served by get()
    quint16 transactStatus = 200;
    int opens = 0;
    int closes = 0;
    struct Write { quint32 cmd; int channel; QByteArray body; };
    QVector<Write> transacts;                 // transact() calls, in order
    QVector<std::tuple<quint32, quint32, int, bool>> enableWrites; // get,set,channel,value
    QHash<quint32, int> enables;              // get cmd -> readEnable() answer (default -1)
    QHash<quint32, QVariantMap> flatConfigs;
    bool writeFieldsOk = true;
    QVector<QVariantMap> fieldWrites;
    BaichuanParams lastParams;
};

class ScriptedLink : public BaichuanLink
{
public:
    explicit ScriptedLink(BaichuanScript &s) : m_s(s) {}
    bool open() override
    {
        ++m_s.opens;
        m_open = m_s.canOpen;
        return m_open;
    }
    void close() override { ++m_s.closes; m_open = false; }
    bool isOpen() const override { return m_open; }
    QByteArray get(quint32 cmdId, int, quint16 *st = nullptr) override
    {
        if (st)
            *st = m_s.replies.contains(cmdId) ? 200 : 400;
        return m_s.replies.value(cmdId);
    }
    int readEnable(quint32 getCmdId, int) override { return m_s.enables.value(getCmdId, -1); }
    bool writeEnable(quint32 g, quint32 s, int ch, bool v) override
    {
        m_s.enableWrites.append({g, s, ch, v});
        return true;
    }
    QVariantMap getConfigFlat(quint32 cmdId, int, const QByteArray &) override
    {
        return m_s.flatConfigs.value(cmdId);
    }
    bool writeFields(quint32, quint32, int, const QVariantMap &changes, const QByteArray &) override
    {
        m_s.fieldWrites.append(changes);
        return m_s.writeFieldsOk;
    }
    QByteArray transact(quint32 cmdId, int channel, const QByteArray &body,
                        quint16 *st = nullptr) override
    {
        m_s.transacts.append({cmdId, channel, body});
        if (st)
            *st = m_s.transactStatus;
        return {};
    }

private:
    BaichuanScript &m_s;
    bool m_open = false;
};

inline BaichuanOpener scriptedOpener(BaichuanScript &s)
{
    return [&s](const BaichuanParams &p) -> std::unique_ptr<BaichuanLink> {
        s.lastParams = p;
        return std::make_unique<ScriptedLink>(s);
    };
}

} // namespace rl::testing
