#include "ui/EventClip.h"

#include "device/DeviceManager.h"

#include <algorithm>

namespace rl {

EventClip::EventClip(QObject *parent) : QObject(parent) {}

DeviceManager *EventClip::devices() const
{
    return m_devices;
}

void EventClip::setDevices(DeviceManager *devices)
{
    if (m_devices == devices)
        return;
    m_devices = devices;
    emit devicesChanged();
}

void EventClip::setClipResolver(ClipResolver resolver)
{
    m_resolver = std::move(resolver);
}

QString EventClip::resolve(qint64 hostId, int channel, qint64 timestamp, int pre, int post) const
{
    if (m_resolver)
        return m_resolver(hostId, channel, timestamp, pre, post);
    return m_devices ? m_devices->eventClipUrl(hostId, channel, timestamp, pre, post) : QString();
}

void EventClip::setEvent(qint64 hostId, int channel, qint64 timestamp)
{
    const QString url = resolve(hostId, channel, timestamp, kFrigatePreSecs, kPostSecs);
    m_active = true;
    m_openEnded = false;
    m_host = hostId;
    m_channel = channel;
    m_ts = timestamp;
    m_url = url;
    m_from = timestamp - (url.isEmpty() ? kPreSecs : kFrigatePreSecs);
    m_to = timestamp + kPostSecs;
    emit changed();
}

void EventClip::keepPlaying()
{
    if (!m_active)
        return;
    m_openEnded = true;
    if (!m_url.isEmpty()) { // Frigate: the next minute as its own clip
        m_url = resolve(m_host, m_channel, m_to, 0, kFrigateMoreSecs);
        m_from = m_to;
        m_to = m_from + kFrigateMoreSecs;
    }
    emit changed();
}

void EventClip::clear()
{
    if (!m_active)
        return;
    m_active = false;
    m_openEnded = false;
    m_host = -1;
    m_channel = 0;
    m_ts = 0;
    m_url.clear();
    m_from = m_to = 0;
    emit changed();
}

double EventClip::progress(double position) const
{
    if (!m_active || m_to <= m_from)
        return 0;
    return std::clamp((position - double(m_from)) / double(m_to - m_from), 0.0, 1.0);
}

bool EventClip::atEnd(double position) const
{
    return m_active && !m_openEnded && position >= double(m_to);
}

} // namespace rl
