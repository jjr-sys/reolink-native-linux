#include "ui/LiveArrangement.h"

#include <QSet>

#include <algorithm>

namespace rl {

namespace {
constexpr int kPresets[] = {1, 4, 6, 9, 16, 20, 25};
}

LiveArrangement::LiveArrangement(QObject *parent)
    : QObject(parent), m_slots(kMaxPanes)
{
    m_giveUp.setSingleShot(true);
    m_giveUp.setInterval(120000);
    connect(&m_giveUp, &QTimer::timeout, this, &LiveArrangement::giveUp);
}

void LiveArrangement::setSaved(const QString &text)
{
    m_saved = m_lastSaved = text;
}

QString LiveArrangement::serialized() const
{
    QStringList out;
    out.reserve(kMaxPanes);
    for (const QString &k : m_slots)
        out << (k.isEmpty() ? QStringLiteral("-") : k);
    return out.join(QLatin1Char(','));
}

void LiveArrangement::setSlots(const QStringList &s)
{
    if (s == m_slots)
        return;
    m_slots = s;
    emit slotsChanged();
    emit pageKeysChanged();
}

// Drop cameras that are gone, remove repeats, then fill gaps with cameras not placed yet.
void LiveArrangement::repair()
{
    const QSet<QString> have(m_cameras.begin(), m_cameras.end());
    QSet<QString> seen;
    QStringList s = m_slots;
    for (QString &k : s) {
        if (k.isEmpty() || !have.contains(k) || seen.contains(k))
            k.clear();
        else
            seen.insert(k);
    }
    QStringList free;
    for (const QString &k : m_cameras)
        if (!seen.contains(k))
            free << k;
    for (QString &k : s) {
        if (free.isEmpty())
            break;
        if (k.isEmpty())
            k = free.takeFirst();
    }
    setSlots(s);
}

void LiveArrangement::restore()
{
    m_restored = true;
    QStringList s(kMaxPanes);
    m_pending.clear();
    if (!m_saved.isEmpty()) {
        const QStringList parts = m_saved.split(QLatin1Char(','));
        const QSet<QString> have(m_cameras.begin(), m_cameras.end());
        QSet<QString> used;
        for (int i = 0; i < kMaxPanes && i < parts.size(); ++i) {
            const QString &k = parts[i];
            if (k.isEmpty() || k == QLatin1String("-"))
                continue;
            if (!have.contains(k))
                m_pending.insert(i, k);
            else if (!used.contains(k)) {
                used.insert(k);
                s[i] = k;
            }
        }
    }
    m_slots = s;
    if (!m_pending.isEmpty())
        m_giveUp.start();
    repair(); // place any camera the saved layout did not cover
    emit slotsChanged();
    emit pageKeysChanged();
    emit restoredChanged();
    if (!m_pending.isEmpty())
        emit pendingChanged();
}

// Cameras arrived: put any saved camera that now exists back in its pane.
void LiveArrangement::applyPending()
{
    if (m_pending.isEmpty())
        return;
    QStringList s = m_slots;
    QHash<int, QString> left;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (!m_cameras.contains(it.value())) {
            left.insert(it.key(), it.value());
            continue;
        }
        for (QString &k : s)
            if (k == it.value())
                k.clear(); // it was standing in elsewhere
        s[it.key()] = it.value();
    }
    m_slots = s;
    m_pending = left;
    if (m_pending.isEmpty()) {
        m_giveUp.stop();
        emit pendingChanged();
    }
}

void LiveArrangement::giveUp()
{
    m_pending.clear();
    emit pendingChanged();
    settle();
}

// After any change: tell QML, and ask for a save when there is something new to store. While
// saved cameras are still awaited nothing is written, so the stored layout is not
// overwritten with stand-ins.
void LiveArrangement::settle()
{
    emit slotsChanged();
    emit pageKeysChanged();
    if (!m_restored || !m_pending.isEmpty())
        return;
    const QString text = serialized();
    if (text == m_lastSaved)
        return;
    m_lastSaved = m_saved = text;
    emit saveRequested(text);
}

void LiveArrangement::setCameras(const QStringList &keys)
{
    QStringList next;
    for (const QString &k : keys)
        if (!k.isEmpty() && !next.contains(k))
            next << k;
    if (next == m_cameras && m_restored)
        return;
    const QStringList prev = m_cameras;
    m_cameras = next;
    emit camerasChanged();
    emit pagingChanged();
    emit pageKeysChanged();

    if (!m_restored) {
        if (m_cameras.isEmpty())
            return;
        restore();
        return;
    }
    // A key that vanished drops its focus; the rest of the arrangement does not move.
    if (!m_maximized.isEmpty() && !m_cameras.contains(m_maximized))
        setMaximized({});
    if (!m_selected.isEmpty() && !m_cameras.contains(m_selected))
        setSelected({});
    applyPending();
    repair();
    settle();
}

void LiveArrangement::reset()
{
    m_giveUp.stop();
    m_pending.clear();
    m_restored = false;
    m_slots = QStringList(kMaxPanes);
    m_saved = m_lastSaved;
    emit slotsChanged();
    emit pageKeysChanged();
    emit pendingChanged();
    emit restoredChanged();
}

bool LiveArrangement::assign(int pane, const QString &key)
{
    if (pane < 0 || pane >= kMaxPanes || !m_cameras.contains(key))
        return false;
    const int from = m_slots.indexOf(key);
    if (from == pane)
        return false;
    const QString displaced = m_slots[pane];
    m_slots[pane] = key;
    if (from >= 0)
        m_slots[from] = displaced; // swap
    settle();
    return true;
}

QString LiveArrangement::keyAt(int pane) const
{
    return pane >= 0 && pane < m_slots.size() ? m_slots[pane] : QString();
}

int LiveArrangement::paneOf(const QString &key) const
{
    return key.isEmpty() ? -1 : m_slots.indexOf(key);
}

void LiveArrangement::setPreset(int n)
{
    if (n == m_preset || std::find(std::begin(kPresets), std::end(kPresets), n) == std::end(kPresets))
        return;
    const bool pageMoved = m_page != 0;
    m_preset = n;
    m_page = 0;
    emit presetChanged();
    emit pagingChanged();
    if (pageMoved)
        emit pageChanged();
    emit pageKeysChanged();
}

int LiveArrangement::pageCount() const
{
    return std::max<qsizetype>(1, (m_cameras.size() + m_preset - 1) / m_preset);
}

int LiveArrangement::curPage() const
{
    return std::min(m_page, pageCount() - 1);
}

void LiveArrangement::setPage(int p)
{
    p = std::max(0, p);
    if (p == m_page)
        return;
    m_page = p;
    emit pagingChanged();
    emit pageChanged();
    emit pageKeysChanged();
}

void LiveArrangement::turnPage(int dir)
{
    const int n = pageCount();
    if (n > 1)
        setPage((curPage() + dir % n + n) % n);
}

QStringList LiveArrangement::pageKeys() const
{
    QStringList out;
    const int first = pageOffset();
    for (int i = first; i < first + m_preset && i < m_slots.size(); ++i)
        if (!m_slots[i].isEmpty())
            out << m_slots[i];
    return out;
}

void LiveArrangement::setMaximized(const QString &key)
{
    if (key == m_maximized)
        return;
    m_maximized = key;
    emit maximizedChanged();
}

void LiveArrangement::setSelected(const QString &key)
{
    if (key == m_selected)
        return;
    m_selected = key;
    emit selectedChanged();
}

QString LiveArrangement::neighbour(const QString &key, int dir, const QStringList &order) const
{
    QStringList list = order;
    if (list.isEmpty())
        for (const QString &k : m_slots)
            if (!k.isEmpty())
                list << k;
    const int i = list.indexOf(key);
    if (list.size() < 2 || i < 0)
        return {};
    const int n = list.size();
    return list[((i + dir) % n + n) % n];
}

QStringList LiveArrangement::savedKeys(const QString &text)
{
    QStringList out;
    for (const QString &k : text.split(QLatin1Char(',')))
        if (!k.isEmpty() && k != QLatin1String("-"))
            out << k;
    return out;
}

QStringList LiveArrangement::dropInto(const QStringList &order, const QString &target,
                                      const QString &source)
{
    QStringList out = order;
    const int ti = out.indexOf(target), si = out.indexOf(source);
    if (ti < 0 || target == source)
        return out;
    if (si < 0)
        out.insert(ti, source);
    else {
        out[si] = target;
        out[ti] = source;
    }
    return out;
}

} // namespace rl
