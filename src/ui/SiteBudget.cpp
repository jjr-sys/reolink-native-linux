#include "ui/SiteBudget.h"

#include "ui/SiteMap.h"
#include "ui/SiteSelection.h"

#include <QHash>

namespace rl {

namespace {
// Sums of decimal Mbit/s: "exactly the budget" must not fail on rounding.
constexpr double kSlack = 1e-6;
} // namespace

SiteBudget::SiteBudget(QObject *parent)
    : QObject(parent)
{
}

void SiteBudget::bump()
{
    ++m_revision;
    emit changed();
}

void SiteBudget::setSites(SiteMap *sites)
{
    if (m_sites == sites)
        return;
    if (m_sites)
        disconnect(m_sites, nullptr, this, nullptr);
    m_sites = sites;
    if (m_sites)
        connect(m_sites, &SiteMap::changed, this, &SiteBudget::bump);
    bump();
}

void SiteBudget::setSelection(SiteSelection *selection)
{
    if (m_selection == selection)
        return;
    if (m_selection)
        disconnect(m_selection, nullptr, this, nullptr);
    m_selection = selection;
    if (m_selection)
        connect(m_selection, &SiteSelection::changed, this, &SiteBudget::bump);
    bump();
}

void SiteBudget::setBudgets(const QVariantMap &budgets)
{
    if (m_budgets == budgets)
        return;
    m_budgets = budgets;
    bump();
}

void SiteBudget::setDefaultBudget(double mbps)
{
    if (qFuzzyCompare(m_defaultBudget, mbps))
        return;
    m_defaultBudget = mbps;
    bump();
}

void SiteBudget::setCosts(const QVariantMap &costs)
{
    if (m_costs == costs)
        return;
    m_costs = costs;
    bump();
}

void SiteBudget::setHd(const QStringList &keys)
{
    if (m_hd == keys)
        return;
    m_hd = keys;
    bump();
}

QString SiteBudget::siteOf(const QString &key) const
{
    return m_sites ? m_sites->siteOfKey(key) : QString();
}

double SiteBudget::budgetOf(const QString &site) const
{
    const double b = m_budgets.value(site).toDouble();
    return b > 0 ? b : m_defaultBudget;
}

double SiteBudget::costOf(const QString &key, bool main) const
{
    const QVariantMap c = m_costs.value(key).toMap();
    const double v = c.value(main ? QStringLiteral("main") : QStringLiteral("sub")).toDouble();
    return v > 0 ? v : main ? kDefaultMainMbps : kDefaultSubMbps;
}

double SiteBudget::used(const QString &site) const
{
    if (!m_selection || site.isEmpty())
        return 0;
    double sum = 0;
    for (const QString &k : m_selection->keys())
        if (siteOf(k) == site)
            sum += costOf(k, m_hd.contains(k));
    return sum;
}

double SiteBudget::needed(const QString &key) const
{
    return costOf(key, m_hd.contains(key));
}

double SiteBudget::hdExtra(const QString &key) const
{
    return costOf(key, true) - costOf(key, false);
}

bool SiteBudget::fits(const QString &site, double extra) const
{
    return site.isEmpty() || used(site) + extra <= budgetOf(site) + kSlack;
}

bool SiteBudget::canAdd(const QString &key) const
{
    if (m_selection && m_selection->contains(key))
        return true;
    return fits(siteOf(key), needed(key));
}

bool SiteBudget::canHd(const QString &key) const
{
    if (m_hd.contains(key))
        return true;
    if (m_selection && !m_selection->contains(key))
        return fits(siteOf(key), costOf(key, true));
    return fits(siteOf(key), hdExtra(key));
}

QStringList SiteBudget::recall(const QStringList &keys)
{
    QHash<QString, double> sums;
    QStringList kept, skipped;
    for (const QString &k : keys) {
        if (kept.contains(k))
            continue;
        const QString site = siteOf(k);
        const double c = needed(k);
        if (site.isEmpty() || sums.value(site) + c <= budgetOf(site) + kSlack) {
            sums[site] += c;
            kept << k;
        } else {
            skipped << k;
        }
    }
    if (m_selection)
        m_selection->replace(kept);
    return skipped;
}

} // namespace rl
