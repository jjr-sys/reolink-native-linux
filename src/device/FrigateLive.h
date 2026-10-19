#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace rl {

// Holds Frigate's /ws websocket open for one server (the one its own web UI uses) and
// hands each new event to the GUI thread the moment Frigate confirms it, instead of up to
// a poll later. Signs in with the host's registered login (frigate::sessionCookie).
// Reconnects with a 5–60 s backoff. DeviceManager keeps polling /api/events too, which
// catches up whatever was missed while the socket was down.
class FrigateLive : public QObject
{
    Q_OBJECT
public:
    FrigateLive(const QString &host, int port, QObject *parent = nullptr);
    // Returns at once; the worker stops on its own and never touches this object again.
    ~FrigateLive() override;

    // At app exit, after deleting the sockets' owners: wait up to `ms` for workers to end.
    static void waitForWorkers(int ms);

    QString host() const { return m_host; }
    int port() const { return m_port; }

signals:
    void connectedChanged(bool on);
    // A new tracked object: Frigate camera name, label ("person", "car", …), start (server epoch s).
    void liveEvent(const QString &camera, const QString &label, double start);

private:
    struct Shared;
    static void run(std::shared_ptr<Shared> shared, QString host, int port);

    QString m_host;
    int m_port = 0;
    std::shared_ptr<Shared> m_shared;
};

} // namespace rl
