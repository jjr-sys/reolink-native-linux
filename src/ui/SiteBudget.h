#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include "ui/SiteMap.h"
#include "ui/SiteSelection.h"

namespace rl {

// The Site budget (wayfinder/site-tabs-redesign): what one page's selected panes cost
// against each Site's link, in Mbit/s. A pane costs its camera's sub stream, or its main
// stream when the camera is in `hd`. No recorder enforces a limit and refusals are silent,
// so the page refuses instead: a camera that would take its Site over the budget can't be
// added, and a pane can't switch to HD over it. Reaching the budget exactly is allowed.
//
// Inputs, all set by QML: the Site map, the page's selection, `budgets` (Site -> Mbit/s,
// `defaultBudget` for the rest), `costs` ("hostId:channel" -> { sub, main } Mbit/s) and `hd`
// (cameras shown in HD). A camera missing from `costs` costs the Reolink default, 0.5 sub /
// 6 HD; a camera with no Site is never refused.
class SiteBudget : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::SiteMap *sites READ sites WRITE setSites NOTIFY changed)
    Q_PROPERTY(rl::SiteSelection *selection READ selection WRITE setSelection NOTIFY changed)
    Q_PROPERTY(QVariantMap budgets READ budgets WRITE setBudgets NOTIFY changed)
    Q_PROPERTY(double defaultBudget READ defaultBudget WRITE setDefaultBudget NOTIFY changed)
    Q_PROPERTY(QVariantMap costs READ costs WRITE setCosts NOTIFY changed)
    Q_PROPERTY(QStringList hd READ hd WRITE setHd NOTIFY changed)
    // Bumped on every change, for QML bindings that call the functions below.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    static constexpr double kDefaultSubMbps = 0.5;
    static constexpr double kDefaultMainMbps = 6.0;

    explicit SiteBudget(QObject *parent = nullptr);

    SiteMap *sites() const { return m_sites; }
    void setSites(SiteMap *sites);
    SiteSelection *selection() const { return m_selection; }
    void setSelection(SiteSelection *selection);
    QVariantMap budgets() const { return m_budgets; }
    void setBudgets(const QVariantMap &budgets);
    double defaultBudget() const { return m_defaultBudget; }
    void setDefaultBudget(double mbps);
    QVariantMap costs() const { return m_costs; }
    void setCosts(const QVariantMap &costs);
    QStringList hd() const { return m_hd; }
    void setHd(const QStringList &keys);
    int revision() const { return m_revision; }

    Q_INVOKABLE double budgetOf(const QString &site) const;
    // What the selected panes at a Site cost now.
    Q_INVOKABLE double used(const QString &site) const;
    Q_INVOKABLE double costOf(const QString &key, bool main) const;
    // What adding this camera's pane would cost (main if it is remembered in HD).
    Q_INVOKABLE double needed(const QString &key) const;
    // What switching this pane to HD adds.
    Q_INVOKABLE double hdExtra(const QString &key) const;
    // Whether the camera's pane can be added (true when already selected).
    Q_INVOKABLE bool canAdd(const QString &key) const;
    // Whether this selected pane can switch to HD (true when already HD).
    Q_INVOKABLE bool canHd(const QString &key) const;

signals:
    void changed();

private:
    void bump();
    QString siteOf(const QString &key) const;
    bool fits(const QString &site, double extra) const;

    QPointer<SiteMap> m_sites;
    QPointer<SiteSelection> m_selection;
    QVariantMap m_budgets, m_costs;
    double m_defaultBudget = 5.0;
    QStringList m_hd;
    int m_revision = 0;
};

} // namespace rl
