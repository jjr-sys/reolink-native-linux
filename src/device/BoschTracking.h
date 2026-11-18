#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace rl {

// Home Assistant's Bosch tracking (bosch-ptz-port ticket 03), read as the Outpost app reads
// it (protocol/.../control/Tracking.kt): the services bosch_camera_{get,set,start,stop}_tracking.

// What a save sends: behaviour "always" | "fixed" | "still", seconds 5..300 in 5 s steps,
// and the position to return to.
struct TrackingSettings {
    QString behaviour;
    int seconds = 0;
    QString returnToken;

    static constexpr int kShortest = 5, kLongest = 300, kStep = 5;
    static bool knownBehaviour(const QString &behaviour);
    // The nearest allowed seconds: a 5 s step within 5..300.
    static int steppedSeconds(int seconds);

    bool operator==(const TrackingSettings &) const = default;
    // seconds as a number, return_token as a string (ticket 01).
    QJsonObject body() const;
};

struct Tracking {
    bool enabled = false;
    // "off", "waiting", "following", "still", "returning", "error" or "unknown".
    QString status;
    TrackingSettings config;
    std::optional<int> remaining;   // seconds left while following or still
    QString error;

    // The phone's words: "Off", "Waiting for a target", "Following · 23 s" …
    QString said() const;

    // Missing or odd fields give nothing: never mistaken for a confirmed Off.
    static std::optional<Tracking> parse(const QJsonObject &response);
};

} // namespace rl
