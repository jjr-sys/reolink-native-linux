#include "ui/StillQueue.h"

#include "core/Log.h"

#include <algorithm>

namespace rl {

void StillQueue::setPerRecorder(int n)
{
    n = std::max(1, n);
    if (n == m_perRecorder)
        return;
    m_perRecorder = n;
    emit perRecorderChanged();
}

void StillQueue::request(const QString &pane, qint64 recorder)
{
    if (m_running.contains(pane)) {
        // Its newer moment replaces the one it is fetching, in the same slot.
        m_startedMs.insert(pane, m_clock.elapsed());
        emit startStill(pane);
        return;
    }
    const auto waitingAlready = std::any_of(m_waiting.cbegin(), m_waiting.cend(),
                                            [&pane](const Entry &e) { return e.pane == pane; });
    if (!waitingAlready)
        m_waiting.append({pane, recorder});
    startNext(recorder);
}

void StillQueue::done(const QString &pane, bool picture)
{
    const auto it = m_running.constFind(pane);
    if (it == m_running.constEnd())
        return;
    qCInfo(lcUi).noquote() << "still" << pane << (picture ? "shown" : "gave up") << "after"
                           << m_clock.elapsed() - m_startedMs.take(pane) << "ms";
    const qint64 recorder = it.value();
    m_running.erase(it);
    startNext(recorder);
}

void StillQueue::cancel(const QString &pane)
{
    m_waiting.removeIf([&pane](const Entry &e) { return e.pane == pane; });
    const auto it = m_running.constFind(pane);
    if (it == m_running.constEnd())
        return;
    const qint64 recorder = it.value();
    m_running.erase(it);
    m_startedMs.remove(pane);
    startNext(recorder);
}

void StillQueue::cancelAll()
{
    m_waiting.clear();
    m_running.clear();
    m_startedMs.clear();
}

int StillQueue::running(qint64 recorder) const
{
    return int(std::count(m_running.cbegin(), m_running.cend(), recorder));
}

void StillQueue::startNext(qint64 recorder)
{
    for (int i = 0; i < m_waiting.size() && running(recorder) < m_perRecorder;) {
        if (m_waiting.at(i).recorder != recorder) {
            ++i;
            continue;
        }
        const QString pane = m_waiting.takeAt(i).pane;
        m_running.insert(pane, recorder);
        m_startedMs.insert(pane, m_clock.elapsed());
        emit startStill(pane);
    }
}

} // namespace rl
