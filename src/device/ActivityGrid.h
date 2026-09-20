#pragma once

#include "device/ActivityPolicy.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>

namespace rl {

class DeviceManager;

// Activity mode for the live grid: runs ActivityPolicy against real (or mock)
// detections and tells QML what each visible tile should show. The user's own
// arrangement is never touched — QML keeps it as the baseline and asks this object
// for per-tile overrides. Off by default; while off nothing is subscribed or ticking.
class ActivityGrid : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // Per visible tile: { row, active, kind, replayFrom (epoch s, 0 = live), trigger (epoch s) }.
    Q_PROPERTY(QVariantList tiles READ tiles NOTIFY tilesChanged)
    Q_PROPERTY(int queued READ queued NOTIFY tilesChanged)

public:
    explicit ActivityGrid(DeviceManager *devices, QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    QVariantList tiles() const { return m_view; }
    int queued() const { return m_policy.queued(); }

    // The camera rows the user's own layout shows in the visible tiles, in tile order.
    Q_INVOKABLE void setBaseline(const QVariantList &rows);
    Q_INVOKABLE void setPinned(int tile, bool pinned) { m_policy.setPinned(tile, pinned); publish(); }
    Q_INVOKABLE void setExcluded(int row, bool excluded) { m_policy.setExcluded(row, excluded); }

    // Feed a detection directly (mock scripts, tests). `type` as DeviceManager emits it.
    void ingest(int row, const QString &type);
    // Started when Activity mode is first switched on.
    // RL_MOCK_ACTIVITY="person:1:3@2;motion:1:4@4": <type>:<hostId>:<channel>@<seconds>.
    void runScript(const QString &script);

signals:
    void enabledChanged();
    void tilesChanged();

private:
    void onDetection(qint64 hostId, int channel, const QString &type);
    void onTick();
    void startScript(const QString &script);
    void publish();
    static qint64 nowMs();

    DeviceManager *m_devices;
    activity::ActivityPolicy m_policy;
    QTimer m_timer;
    bool m_enabled = false;
    QVariantList m_view;
    QVector<qint64> m_replayFrom; // per tile, ms, 0 = live
    QVector<qint64> m_trigger;
    QString m_pendingScript; // mock script, started the first time Activity mode is on
};

} // namespace rl
