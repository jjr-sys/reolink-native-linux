#include "device/HomeAssistantTransport.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace rl {

NetworkHomeAssistantTransport::NetworkHomeAssistantTransport(QObject *parent, int timeoutMs)
    : HomeAssistantTransport(parent), m_network(new QNetworkAccessManager(this)),
      m_timeoutMs(timeoutMs)
{
}

void NetworkHomeAssistantTransport::send(const QUrl &url, const QString &token,
                                         const QByteArray &body, Reply reply)
{
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    // The Lascelles reverse proxy blocks Python's default user-agent but
    // accepts curl's.
    req.setRawHeader("User-Agent", "curl/8.5.0");
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(m_timeoutMs);
    if (!body.isNull())
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *r = body.isNull() ? m_network->get(req) : m_network->post(req, body);
    const int seconds = (m_timeoutMs + 999) / 1000;
    connect(r, &QNetworkReply::finished, this, [r, seconds, reply = std::move(reply)]() {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = r->readAll();
        const QString error = r->error() == QNetworkReply::NoError ? QString()
            : r->error() == QNetworkReply::TimeoutError
                ? QStringLiteral("No answer from Home Assistant in %1 s").arg(seconds)
                : r->errorString();
        r->deleteLater();
        if (reply)
            reply(status, data, error);
    });
}

} // namespace rl
