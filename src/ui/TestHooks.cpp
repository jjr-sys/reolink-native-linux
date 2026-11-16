#include "ui/TestHooks.h"

#include <QtGlobal>

namespace rl {

namespace {

bool isSet(const char *name) { return qEnvironmentVariableIsSet(name); }
int number(const char *name) { return qEnvironmentVariableIntValue(name); }
QString text(const char *name) { return qEnvironmentVariable(name); }

} // namespace

TestHooks::TestHooks(QObject *parent)
    : QObject(parent),
      m_initialPage(number("RL_INITIAL_PAGE")),
      m_initialSettingsCategory(text("RL_SETTINGS_CATEGORY")),
      m_eventsAutoScroll(isSet("RL_EVENTS_AUTOSCROLL")),
      m_eventsAutoPlay(isSet("RL_EVENTS_PLAY")),
      m_eventsKeys(text("RL_EVENTS_KEYS")),
      m_mockRecordings(isSet("RL_MOCK_RECORDINGS")),
      m_mockDoorbell(isSet("RL_MOCK_DOORBELL")),
      m_mockDoorbellTarget(text("RL_MOCK_DOORBELL")),
      m_openClipTarget(text("RL_OPEN_CLIP")),
      m_playbackAutoplay(isSet("RL_PLAYBACK_AUTOPLAY")),
      m_playbackAtSecs(isSet("RL_PLAYBACK_AT") ? number("RL_PLAYBACK_AT") : -1),
      m_playbackStopMs(number("RL_PLAYBACK_STOP_MS")),
      m_playbackHd(isSet("RL_PLAYBACK_HD")),
      m_playbackBackSteps(number("RL_PLAYBACK_BACK")),
      m_playbackFastSteps(number("RL_PLAYBACK_FAST")),
      m_paneBarsShown(isSet("RL_PANE_BARS")),
      m_liveReplayTest(text("RL_LIVE_REPLAY"))
{
}

} // namespace rl
