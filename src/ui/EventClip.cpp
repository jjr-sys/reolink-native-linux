#include "ui/EventClip.h"

#include "device/DeviceManager.h"

#include <algorithm>

namespace rl {

EventClip::EventClip(QObject *parent) : QObject(parent), m_group(new PlaybackGroup(this)) {}

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
    m_group->setReverseAvailable(url.isEmpty());   // a Frigate file plays forwards only
    emit changed();
}

void EventClip::extendWindow()
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
    if (!m_active) {
        setPhase(Idle);
        return;
    }
    m_active = false;
    m_openEnded = false;
    m_host = -1;
    m_channel = 0;
    m_ts = 0;
    m_url.clear();
    m_from = m_to = 0;
    emit changed();
    setPhase(Idle);
}

// ---- The state machine (architecture review #2, candidate 2) ---------------------------------

void EventClip::setPhase(Phase phase)
{
    if (m_phase == phase)
        return;   // reopening while starting keeps the first deadline
    if (phase == Starting)
        m_startingSince = -1;   // the give-up count starts at the next tick
    m_phase = phase;
    emit phaseChanged();
    emit positionChanged();     // shownPosition follows the phase
}

double EventClip::position() const
{
    return frigate() ? double(m_from) + m_mediaPositionMs / 1000 : m_clock;
}

void EventClip::setFrameEpochMs(double ms)
{
    if (m_frameEpochMs == ms)
        return;
    m_frameEpochMs = ms;
    emit positionChanged();
}

void EventClip::setMediaPositionMs(double ms)
{
    if (m_mediaPositionMs == ms)
        return;
    m_mediaPositionMs = ms;
    emit positionChanged();
    checkEdge();
}

void EventClip::play(qint64 hostId, int channel, qint64 timestamp)
{
    setPhase(Idle);   // first: the old position must not end the new window
    m_hadPicture = false;
    emit stopAllWanted();
    m_group->playForward();   // a new event plays at 1× forward
    emit retryHdWanted();     // and tries HD again
    setEvent(hostId, channel, timestamp);
    open(double(m_from));
}

void EventClip::close()
{
    emit stopAllWanted();
    clear();   // and back to Idle
}

void EventClip::open(double at)
{
    m_openedAt = at;
    m_clock = at;
    // Frigate loads its file from the start: the last file's playhead must not carry over.
    if (frigate())
        m_mediaPositionMs = 0;
    m_epochAtOpenMs = m_frameEpochMs;
    setPhase(Starting);
    emit positionChanged();
    emit openWanted(at);
}

void EventClip::keepPlaying()
{
    if (!m_active)
        return;
    const double from = position();
    extendWindow();
    open(frigate() ? double(m_from) : from);
}

void EventClip::togglePause()
{
    if (m_phase == Playing) {
        m_pausedAt = position();
        if (frigate())
            emit pauseMediaWanted();
        else
            emit stopStreamWanted();
        setPhase(Paused);
    } else if (m_phase == Paused) {
        seekTo(m_pausedAt);
    } else if (m_phase == Ended) {
        seekTo(double(m_from));   // from the window's start again
    }
}

void EventClip::seekTo(double at)
{
    if (!m_active)
        return;
    at = skipFrom(at, 0);
    emit retryHdWanted();
    m_lastReopenAt = -1;
    // Backwards from the window's start: nothing to play, stay there.
    if (!frigate() && direction() < 0 && edgeReached(at, -1)) {
        stopAtStart();
        return;
    }
    if (frigate()) {
        emit seekMediaWanted(std::max(0.0, (at - double(m_from)) * 1000));
        setPhase(Playing);
        return;
    }
    emit stopStreamWanted();
    open(at);
}

void EventClip::skip(double secs)
{
    seekTo(skipFrom(m_phase == Paused ? m_pausedAt : position(), secs));
}

void EventClip::speedChanged()
{
    if (m_phase == Paused)
        seekTo(m_pausedAt);
    else if (m_phase == Ended)
        seekTo(direction() < 0 ? position() : double(m_from));
}

void EventClip::hdChanged()
{
    // Reopen, not seek: a running stream moved in place would stay on the sub stream.
    if (!m_active || frigate() || (m_phase != Playing && m_phase != Starting))
        return;
    const double at = position();
    emit stopStreamWanted();
    open(at);
}

double EventClip::handOffAt() const
{
    if (m_phase == Paused)
        return m_pausedAt;
    if (m_phase == Playing || m_phase == Ended)
        return position();
    return double(m_ts);
}

bool EventClip::firstPicture(double wallMs)
{
    if (m_phase != Starting)
        return false;
    m_hadPicture = true;
    m_lastTickWall = wallMs;
    setPhase(Playing);
    return true;
}

void EventClip::streamReconnecting()
{
    // Back to starting: the clock restarts with the picture.
    if (m_phase == Playing)
        setPhase(Starting);
}

void EventClip::streamStopped(bool stillStopped)
{
    if (m_phase != Playing || frigate() || !stillStopped)
        return;
    const double at = reopenAfterEnd(position(), direction(), m_lastReopenAt);
    if (at < 0) {
        if (direction() < 0)
            stopAtStart();
        else
            reachEnd();
        return;
    }
    open(at);
    m_lastReopenAt = at;
}

void EventClip::mediaEnded(double nowSecs)
{
    if (m_phase != Playing)
        return;
    // Kept playing: the next minute, while it is in the past; else the end.
    if (m_openEnded && double(m_to) <= nowSecs - 5) {
        extendWindow();
        open(double(m_from));
    } else {
        reachEnd();
    }
}

void EventClip::loadFailed()
{
    if (m_phase == Starting)
        setPhase(Failed);
}

void EventClip::openFailed()
{
    setPhase(Failed);
}

void EventClip::reopenNeeded()
{
    if (m_phase == Playing || m_phase == Starting)
        open(position());
}

void EventClip::tick(double wallMs)
{
    if (m_phase == Starting) {
        if (m_startingSince < 0) {
            m_startingSince = wallMs;
            return;
        }
        const int limit = frigate() ? kGiveUpFrigateMs : m_group->offNormal() ? kGiveUpOffNormalMs : kGiveUpMs;
        if (wallMs - m_startingSince >= limit)
            giveUp();
        return;
    }
    if (m_phase != Playing || frigate())
        return;
    // Off 1× forwards the playhead follows the frame on screen (a fresh one, not the frame
    // from before the open), as Playback's does; else it runs with the picture at the group's
    // speed and direction. A speed change never rewrites it.
    const double dt = std::max(0.0, (wallMs - m_lastTickWall) / 1000);
    m_lastTickWall = std::max(m_lastTickWall, wallMs);
    const bool frameTimed = m_group->offNormal() && m_frameEpochMs > 0 && m_frameEpochMs != m_epochAtOpenMs;
    m_clock = frameTimed ? m_frameEpochMs / 1000 : m_clock + dt * m_group->speed() * direction();
    emit positionChanged();
    checkEdge();
}

void EventClip::giveUp()
{
    // With a picture already up, a reopen that doesn't come pauses on it instead of failing.
    const double at = m_openedAt;
    emit stopAllWanted();
    if (m_hadPicture) {
        m_pausedAt = at;
        setPhase(Paused);
        emit notice(PartMissing);
    } else {
        setPhase(Failed);
    }
}

void EventClip::checkEdge()
{
    if (m_phase != Playing || !edgeReached(position(), frigate() ? 1 : direction()))
        return;
    if (!frigate() && direction() < 0)
        stopAtStart();
    else
        reachEnd();
}

void EventClip::stopAtStart()
{
    // Backwards at the window's start, or nothing earlier in the recording: pause there.
    m_pausedAt = std::max(double(m_from), position());
    emit stopStreamWanted();
    setPhase(Paused);
    emit notice(StartOfClip);
}

void EventClip::reachEnd()
{
    if (!frigate())
        emit stopStreamWanted();
    setPhase(Ended);
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

double EventClip::timeAt(double fraction) const
{
    if (!m_active)
        return 0;
    return double(m_from) + std::clamp(fraction, 0.0, 1.0) * double(m_to - m_from);
}

bool EventClip::edgeReached(double position, int direction) const
{
    if (!m_active)
        return false;
    if (direction < 0)
        return position <= double(m_from) + 0.25;
    return atEnd(position);
}

double EventClip::reopenAfterEnd(double position, int direction, double lastReopenAt) const
{
    if (!m_active)
        return -1;
    const int dir = direction < 0 ? -1 : 1;
    if (edgeReached(position + dir, dir))
        return -1;
    if (lastReopenAt >= 0 && dir * (position - lastReopenAt) <= 0.5)
        return -1;   // ended again where it reopened: nothing more that way
    return dir < 0 ? position : position + 1;
}

double EventClip::skipFrom(double position, double secs) const
{
    if (!m_active)
        return 0;
    const double to = position + secs;
    return m_openEnded ? std::max(double(m_from), to) : std::clamp(to, double(m_from), double(m_to));
}

} // namespace rl
