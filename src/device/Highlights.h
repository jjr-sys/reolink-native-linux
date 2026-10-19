#pragma once

#include "device/HighlightPolicy.h"

#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace rl {

class DeviceManager;

// Highlights for Live View: runs HighlightPolicy against real (or mock) detections and
// tells QML how each camera's panes look. One switch turns the whole feature (borders and
// Alert sounds) on or off; while off nothing is highlighted and no sound is asked for.
class Highlights : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // "hostId:channel" -> { kind, colour, style, since, flashUntil, last } (times in epoch ms).
    Q_PROPERTY(QVariantMap looks READ looks NOTIFY looksChanged)
    // The Highlight rules as JSON (see rulesFromJson), saved in [highlights] rules.
    Q_PROPERTY(QString rules READ rules WRITE setRules NOTIFY rulesChanged)

public:
    explicit Highlights(DeviceManager *devices, QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    QVariantMap looks() const { return m_looks; }
    QString rules() const;
    void setRules(const QString &json);

    // Feed a detection directly (mock scripts, tests). `type` as DeviceManager emits it.
    Q_INVOKABLE void ingest(const QString &camera, const QString &type);
    // RL_MOCK_DETECTIONS="person:1:3@2;motion:1:4@4": <type>:<hostId>:<channel>@<seconds>.
    void runScript(const QString &script);

signals:
    void enabledChanged();
    void looksChanged();
    void rulesChanged();
    // A Highlight rule asks for an Alert sound (played by the alert-sound player).
    void soundRequested(const QString &sound, const QString &camera, const QString &kind);

private:
    void publish();
    static qint64 nowMs();

    highlight::HighlightPolicy m_policy;
    QTimer m_timer;
    bool m_enabled = true;
    QVariantMap m_looks;
};

} // namespace rl
