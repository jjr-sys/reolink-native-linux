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

// Production adapter: QNetworkAccessManager.
class NetworkHomeAssistantTransport : public HomeAssistantTransport
{
    Q_OBJECT
public:
    explicit NetworkHomeAssistantTransport(QObject *parent = nullptr);
    void send(const QUrl &url, const QString &token, const QByteArray &body,
              Reply reply) override;

private:
    QNetworkAccessManager *m_network = nullptr;
};

} // namespace rl
