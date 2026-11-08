#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>

#include "ui/SiteMap.h"

namespace rl {

// One page's selection of cameras on the Site tabs (wayfinder/site-tabs-redesign): tapping
// a camera adds or removes its pane. Cameras are keys, "hostId:channel".
//
//  - `keys` is tap order. `paneOrder` is what the page shows: grouped by Site in Site tab
//    order (from `sites`), tap order within a Site; cameras of an unknown host go last.
//  - Keys whose camera has not appeared yet are kept (recorders answer late); only
//    pruneHosts drops keys, for recorders that no longer exist.
//  - clear() offers undo() until the next change or dropUndo().
//  - Give it the stored text first (setSaved); saveRequested(text) says a change by the
//    user is worth storing. Stored text: keys in tap order joined by ",".
class SiteSelection : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::SiteMap *sites READ sites WRITE setSites NOTIFY sitesChanged)
    Q_PROPERTY(QString saved READ saved WRITE setSaved)
    Q_PROPERTY(QStringList keys READ keys NOTIFY changed)
    Q_PROPERTY(QStringList paneOrder READ paneOrder NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(int undoCount READ undoCount NOTIFY undoChanged)

public:
    explicit SiteSelection(QObject *parent = nullptr);

    SiteMap *sites() const { return m_sites; }
    void setSites(SiteMap *sites);
    QString saved() const { return m_saved; }
    void setSaved(const QString &text);
    QString serialized() const { return m_keys.join(QLatin1Char(',')); }
    QStringList keys() const { return m_keys; }
    QStringList paneOrder() const { return m_paneOrder; }
    int count() const { return int(m_keys.size()); }
    bool canUndo() const { return !m_undo.isEmpty(); }
    int undoCount() const { return int(m_undo.size()); }

    Q_INVOKABLE bool contains(const QString &key) const { return m_keys.contains(key); }
    Q_INVOKABLE void toggle(const QString &key);
    // Returns how many cameras were cleared.
    Q_INVOKABLE int clear();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void dropUndo();
    Q_INVOKABLE bool sameSite(const QString &a, const QString &b) const;
    // Swap two selected cameras' places; refused across Sites.
    Q_INVOKABLE bool swap(const QString &a, const QString &b);
    // The camera `dir` panes from `key` in pane order, wrapping; "" with nowhere to go.
    Q_INVOKABLE QString neighbour(const QString &key, int dir) const;
    Q_INVOKABLE int countIn(const QString &site) const;
    // Drop the keys of hosts not in `hostIds`.
    Q_INVOKABLE void pruneHosts(const QVariantList &hostIds);

    // The first-launch selection from the old Live grid: the panes of page one of the
    // stored arrangement ("hostId:channel" or "-" per pane) at `preset` panes a page.
    Q_INVOKABLE static QStringList fromGrid(const QString &arrangement, int preset);

signals:
    void sitesChanged();
    void changed();
    void undoChanged();
    void saveRequested(const QString &text);

private:
    static QStringList parse(const QString &text);
    void setKeys(const QStringList &keys, bool save);
    void regroup();

    QPointer<SiteMap> m_sites;
    QString m_saved;
    QStringList m_keys, m_paneOrder, m_undo;
};

} // namespace rl
