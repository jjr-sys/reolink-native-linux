#include "device/OfflineAlerts.h"

#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>

namespace rl::device {

namespace {
QString tr(const char *text) { return QCoreApplication::translate("OfflineAlerts", text); }

QString clock(qint64 at)
{
    return QDateTime::fromSecsSinceEpoch(at).toString(QStringLiteral("HH:mm"));
}

// "45 s", "4 min", "2 h 5 min".
QString span(qint64 secs)
{
    if (secs < 60)
        return tr("%1 s").arg(secs);
    const qint64 mins = secs / 60;
    if (mins < 60)
        return tr("%1 min").arg(mins);
    return tr("%1 h %2 min").arg(mins / 60).arg(mins % 60);
}
} // namespace

QString OfflineAlerts::keyOf(qint64 hostId, int channel)
{
    return QStringLiteral("%1:%2").arg(hostId).arg(channel);
}

QVector<OfflineAlert> OfflineAlerts::change(const Change &c, qint64 now)
{
    const QString key = keyOf(c.hostId, c.channel);
    Track &t = m_tracks[key];
    t.name = c.name;
    t.hostId = c.hostId;
    // A lone camera's host opens that camera; a recorder opens the recorder.
    t.channel = c.channel >= 0 ? c.channel
              : c.recorder     ? -1
                               : (c.cameras.isEmpty() ? 0 : c.cameras.first().channel);
    t.recorder = c.channel < 0 && c.recorder;
    if (!c.online && t.recorder && !c.cameras.isEmpty())
        t.cameraCount = int(c.cameras.size());

    const auto alert = [&](OfflineAlert::Kind kind, const QString &title, const QString &body,
                           bool critical) {
        return QVector<OfflineAlert>{{kind, key, t.hostId, t.channel, title, body, critical}};
    };

    if (!c.online) {
        if (t.down)
            return {};
        t.down = true;
        t.drops.erase(std::remove_if(t.drops.begin(), t.drops.end(),
                                     [&](qint64 at) { return at <= now - m_timing.flapWindowSecs; }),
                      t.drops.end());
        t.drops.append(now);

        // A recorder down covers its cameras still in their grace.
        if (c.channel < 0) {
            for (auto it = m_tracks.begin(); it != m_tracks.end(); ++it)
                if (it->hostId == c.hostId && it.key() != key && it->state == State::Waiting)
                    it->state = State::Up;
        }

        if (t.state == State::Unstable)
            return {};
        if (t.drops.size() >= m_timing.flapDrops) {
            t.state = State::Unstable;
            return alert(OfflineAlert::Kind::Unstable, tr("%1 unstable").arg(t.name),
                         tr("Dropped %1 times in %2. No more alerts until it stays up %3.")
                             .arg(t.drops.size())
                             .arg(span(m_timing.flapWindowSecs))
                             .arg(span(m_timing.stableSecs)),
                         true);
        }
        t.since = now;
        if (c.channel < 0 && !c.atStart) {   // the host check already waited out the grace
            t.state = State::Alerted;
            return alert(OfflineAlert::Kind::Offline, offlineTitle(t), offlineBody(t), true);
        }
        t.state = State::Waiting;
        return {};
    }

    if (!t.down)
        return {};
    t.down = false;
    switch (t.state) {
    case State::Waiting:
        t.state = State::Up;
        return {};
    case State::Alerted:
        t.state = State::Up;
        return alert(OfflineAlert::Kind::BackOnline, tr("%1 back online").arg(t.name),
                     tr("Back after %1").arg(span(now - t.since)), false);
    case State::Unstable:
        t.since = now;
        return {};
    case State::Up:
        return {};
    }
    return {};
}

QVector<OfflineAlert> OfflineAlerts::tick(qint64 now)
{
    QVector<OfflineAlert> out;
    for (auto it = m_tracks.begin(); it != m_tracks.end(); ++it) {
        Track &t = it.value();
        if (t.state == State::Waiting && now >= t.since + m_timing.graceSecs) {
            t.state = State::Alerted;
            out.append({OfflineAlert::Kind::Offline, it.key(), t.hostId, t.channel,
                        offlineTitle(t), offlineBody(t), true});
        } else if (t.state == State::Unstable && !t.down && now >= t.since + m_timing.stableSecs) {
            t.state = State::Up;
            t.drops.clear();
            out.append({OfflineAlert::Kind::Stable, it.key(), t.hostId, t.channel,
                        tr("%1 stable again").arg(t.name),
                        tr("Up for %1").arg(span(m_timing.stableSecs)), false});
        }
    }
    return out;
}

qint64 OfflineAlerts::nextDue() const
{
    qint64 due = -1;
    for (const Track &t : m_tracks) {
        qint64 at = -1;
        if (t.state == State::Waiting)
            at = t.since + m_timing.graceSecs;
        else if (t.state == State::Unstable && !t.down)
            at = t.since + m_timing.stableSecs;
        if (at >= 0 && (due < 0 || at < due))
            due = at;
    }
    return due;
}

QString OfflineAlerts::offlineTitle(const Track &t)
{
    if (!t.recorder || t.cameraCount <= 0)
        return tr("%1 offline").arg(t.name);
    return t.cameraCount == 1 ? tr("%1 offline · 1 camera").arg(t.name)
                              : tr("%1 offline · %2 cameras").arg(t.name).arg(t.cameraCount);
}

QString OfflineAlerts::offlineBody(const Track &t)
{
    return t.recorder ? tr("Recorder unreachable since %1").arg(clock(t.since))
                      : tr("Camera offline since %1").arg(clock(t.since));
}

} // namespace rl::device
