#pragma once

namespace rl {

// "No picture" for a live pane (wayfinder/site-tabs-redesign). Recorders refuse a stream
// silently: the session opens and then nothing arrives. If no picture comes within 8 s of
// opening, or 8 s after the last one, the pane shows "No picture: retrying" and the stream
// is reopened, at growing intervals: 8, 16, 32 s, then every 60 s, until a picture arrives.
// Time is whatever advance() is told, so it holds no clock.
class PictureWatch
{
public:
    // A connection was opened (fresh, or a reopen this asked for): the wait starts again.
    // A run of failures is kept, so the wait keeps growing.
    void start() { m_elapsed = 0; }
    // A new picture arrived: all is well again.
    void frame()
    {
        m_elapsed = 0;
        m_failures = 0;
        m_noPicture = false;
    }
    // The stream was stopped or changed on purpose: forget everything.
    void reset()
    {
        m_elapsed = 0;
        m_failures = 0;
        m_noPicture = false;
    }
    // `ms` passed with no picture. True when the stream should be reopened now.
    bool advance(int ms)
    {
        m_elapsed += ms;
        if (m_elapsed < window())
            return false;
        m_noPicture = true;
        ++m_failures;
        m_elapsed = 0;
        return true;
    }
    bool noPicture() const { return m_noPicture; }
    // How long the current wait is.
    int window() const
    {
        static constexpr int kSteps[] = {8000, 16000, 32000};
        return m_failures < 3 ? kSteps[m_failures] : 60000;
    }

private:
    int m_elapsed = 0;
    int m_failures = 0;
    bool m_noPicture = false;
};

} // namespace rl
