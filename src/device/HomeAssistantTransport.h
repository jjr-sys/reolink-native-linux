#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

namespace rl {

// Seam between BoschTourController's state machines and the Home Assistant
// HTTP API. One method: send a bearer-authenticated request (GET when body is
// null, POST with a JSON body otherwise) and report (HTTP status, body,
// network error text) exactly once. Status is 0 when no HTTP reply arrived.
class HomeAssistantTransport : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QByteArray &body, const QString &error)>;

    explicit HomeAssistantTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~HomeAssistantTransport() override = default;

    virtual void send(const QUrl &url, const QString &token, const QByteArray &body,
                      Reply reply) = 0;
};

// Production adapter: QNetworkAccessManager. A request with no answer within the limit
// ends with status 0 and "No answer from Home Assistant in N s", so a lost request can't
// leave a control waiting forever. The limit covers the slowest service: the first manual
// move while tracking is on waits up to ~24 s for tracking to confirm Off.
class NetworkHomeAssistantTransport : public HomeAssistantTransport
{
    Q_OBJECT
public:
    static constexpr int kDefaultTimeoutMs = 40000;

    explicit NetworkHomeAssistantTransport(QObject *parent = nullptr,
                                           int timeoutMs = kDefaultTimeoutMs);
    void send(const QUrl &url, const QString &token, const QByteArray &body,
              Reply reply) override;

private:
    QNetworkAccessManager *m_network = nullptr;
    int m_timeoutMs = kDefaultTimeoutMs;
};

} // namespace rl
