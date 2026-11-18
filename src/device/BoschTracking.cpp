#include "device/BoschTracking.h"

#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace rl {

namespace {

// A whole JSON number (not a string), as the phone's intOrNull.
std::optional<int> wholeNumber(const QJsonValue &value)
{
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (std::floor(number) != number || std::abs(number) > 1e9)
        return std::nullopt;
    return int(number);
}

std::optional<QString> text(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (!value.isString())
        return std::nullopt;
    return value.toString();
}

} // namespace

bool TrackingSettings::knownBehaviour(const QString &behaviour)
{
    return behaviour == QLatin1String("always") || behaviour == QLatin1String("fixed")
           || behaviour == QLatin1String("still");
}

int TrackingSettings::steppedSeconds(int seconds)
{
    return std::clamp(int(std::lround(seconds / double(kStep))) * kStep, kShortest, kLongest);
}

QJsonObject TrackingSettings::body() const
{
    return {{QStringLiteral("behaviour"), behaviour},
            {QStringLiteral("seconds"), seconds},
            {QStringLiteral("return_token"), returnToken}};
}

QString Tracking::said() const
{
    QString words;
    if (status == QLatin1String("off"))
        words = QStringLiteral("Off");
    else if (status == QLatin1String("waiting"))
        words = QStringLiteral("Waiting for a target");
    else if (status == QLatin1String("following"))
        words = QStringLiteral("Following");
    else if (status == QLatin1String("still"))
        words = QStringLiteral("Still");
    else if (status == QLatin1String("returning"))
        words = QStringLiteral("Returning");
    else if (status == QLatin1String("error"))
        words = error.isEmpty() ? QStringLiteral("Tracking needs attention") : error;
    else
        words = QStringLiteral("Status unavailable");
    if (remaining && (status == QLatin1String("following") || status == QLatin1String("still")))
        words += QStringLiteral(" · %1 s").arg(*remaining);
    return words;
}

std::optional<Tracking> Tracking::parse(const QJsonObject &response)
{
    static const QStringList statuses{
        QStringLiteral("off"),   QStringLiteral("waiting"),   QStringLiteral("following"),
        QStringLiteral("still"), QStringLiteral("returning"), QStringLiteral("error"),
        QStringLiteral("unknown")};
    const QJsonValue enabled = response.value(QStringLiteral("enabled"));
    if (!enabled.isBool())
        return std::nullopt;
    const auto status = text(response, QStringLiteral("status"));
    if (!status || !statuses.contains(*status))
        return std::nullopt;
    if (*status == QLatin1String("off") && enabled.toBool())
        return std::nullopt;
    const QJsonValue configValue = response.value(QStringLiteral("config"));
    if (!configValue.isObject())
        return std::nullopt;
    const QJsonObject config = configValue.toObject();
    const auto behaviour = text(config, QStringLiteral("behaviour"));
    const auto seconds = wholeNumber(config.value(QStringLiteral("seconds")));
    const auto token = text(config, QStringLiteral("return_token"));
    if (!behaviour || !TrackingSettings::knownBehaviour(*behaviour) || !seconds
        || TrackingSettings::steppedSeconds(*seconds) != *seconds || !token
        || token->trimmed().isEmpty())
        return std::nullopt;

    Tracking tracking;
    tracking.enabled = enabled.toBool();
    tracking.status = *status;
    tracking.config = {*behaviour, *seconds, *token};
    const auto remaining = wholeNumber(response.value(QStringLiteral("remaining_seconds")));
    if (remaining && *remaining >= 0)
        tracking.remaining = remaining;
    tracking.error = text(response, QStringLiteral("error")).value_or(QString());
    return tracking;
}

} // namespace rl
