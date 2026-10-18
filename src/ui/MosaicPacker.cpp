#include "MosaicPacker.h"

#include <algorithm>
#include <cmath>

namespace rl::mosaic {

namespace {
constexpr double kLnMax = 8.0; // ln(aspect) is clamped to ±8 for bucketing
constexpr double kInf = std::numeric_limits<double>::infinity();

bool byAspect(const Cand &x, const Cand &y) { return x.a < y.a; }
int bucketOf(double a, double delta)
{
    return int(std::floor((std::clamp(std::log(a), -kLnMax, kLnMax) + kLnMax) / delta));
}
} // namespace

void Packer::build(const std::vector<Pane> &panes)
{
    m_panes = panes;
    m_n = int(panes.size());
    m_runs = 0;
    // The floor can leave no layout at all (several 32:9 panes, say); halve it until one
    // survives, ending at 0, which always does.
    double f = m_p.evenFloor;
    for (;;) {
        run(f);
        if (m_n == 0 || !roots().empty() || f == 0)
            break;
        f = f < 0.05 ? 0 : f / 2;
    }
}

int Packer::layout(double W, double H, double gap, std::vector<Rect> &out, double *score)
{
    out.assign(m_n, {});
    if (m_n == 0)
        return -1;
    double best;
    int k = pick(W, H, gap, best);
    // Certificate: evenness never rises going up the tree and fill <= 1, so anything the
    // floor removed scores below floor^evenExp. A winner below that reruns with its own
    // score as the floor, which makes the answer identical to the unpruned search.
    if (m_floor > 0 && best < std::pow(m_floor, m_p.evenExp)) {
        run(std::pow(best, 1.0 / m_p.evenExp) * 0.999);
        k = pick(W, H, gap, best);
    }
    place(k, W, H, gap, out);
    if (score)
        *score = best;
    return k;
}

double Packer::scoreOf(int root, double W, double H, double gap) const
{
    return score(roots()[root], W, H, gap);
}

void Packer::place(int root, double W, double H, double gap, std::vector<Rect> &out) const
{
    out.assign(m_n, {});
    const Cand &c = roots()[root];
    const double h = std::min(H, (W - c.beta * gap) / c.a), w = c.a * h + c.beta * gap;
    placeRun(0, m_n - 1, root, (W - w) / 2, (H - h) / 2, w, h, gap, out);
}

std::vector<int> Packer::leaves(int root) const
{
    std::vector<int> l;
    walk(0, m_n - 1, root, l);
    return l;
}

// Follows the stored child links; a leaf records its own pane index.
void Packer::walk(int i, int j, int k, std::vector<int> &leaves) const
{
    const Cand &c = m_cands[idx(i, j)][k];
    if (c.ia < 0) {
        leaves.push_back(c.split);
        return;
    }
    walk(i, c.split, c.ia, leaves);
    walk(c.split + 1, j, c.ib, leaves);
}

void Packer::run(double floor)
{
    ++m_runs;
    m_floor = floor;
    m_cands.assign(m_n * m_n, {});
    for (int i = 0; i < m_n; ++i) {
        const double s = 1.0 / m_panes[i].weight;
        Cand c;
        c.a = m_panes[i].ratio;
        c.mn = c.mx = s;
        c.split = int16_t(i);
        if (m_panes[i].weight > 1)
            c.lm = s;
        m_cands[idx(i, i)].push_back(c);
    }
    for (int len = 2; len <= m_n; ++len)
        for (int i = 0; i + len - 1 < m_n; ++i)
            combine(i, i + len - 1, len == m_n ? m_p.rootDelta : m_p.delta);
    // Roots are scanned in aspect order, so ties break the same way whatever the floor.
    if (m_n > 1) {
        auto &r = m_cands[idx(0, m_n - 1)];
        std::stable_sort(r.begin(), r.end(), byAspect);
    }
}

void Packer::combine(int i, int j, double delta)
{
    const int nb = int(std::ceil(2 * kLnMax / delta)) + 2;
    if (int(m_table.size()) < nb)
        m_table.assign(nb, -1);
    if (m_p.largeReps && int(m_tableLarge.size()) < nb)
        m_tableLarge.assign(nb, -1);
    m_used.clear();
    m_usedLarge.clear();
    std::vector<Cand> &res = m_cands[idx(i, j)];
    std::vector<Cand> large;
    res.clear();

    // Keep one representative per ln(aspect) bucket.
    auto add = [&](const Cand &c) {
        if (c.mn / c.mx < m_floor)
            return;
        const int k = bucketOf(c.a, delta);
        int &slot = m_table[k];
        if (slot < 0) {
            slot = int(res.size());
            res.push_back(c);
            m_used.push_back(k);
        } else if (better(c, res[slot])) {
            res[slot] = c;
        }
        if (m_p.largeReps && c.lm < kInf) {
            int &s2 = m_tableLarge[k];
            if (s2 < 0) {
                s2 = int(large.size());
                large.push_back(c);
                m_usedLarge.push_back(k);
            } else if (betterLarge(c, large[s2])) {
                large[s2] = c;
            }
        }
    };

    const double f = m_floor;
    for (int s = i; s < j; ++s) {
        const auto &A = m_cands[idx(i, s)];
        const auto &B = m_cands[idx(s + 1, j)]; // sorted by aspect
        double maxMnB = 0, minMxB = 1e300;
        for (const Cand &b : B) {
            maxMnB = std::max(maxMnB, b.mn);
            minMxB = std::min(minMxB, b.mx);
        }
        auto lower = [&](double v) {
            return int(std::lower_bound(B.begin(), B.end(), v,
                                        [](const Cand &c, double x) { return c.a < x; }) -
                       B.begin());
        };
        auto upper = [&](double v) {
            return int(std::upper_bound(B.begin(), B.end(), v,
                                        [](double x, const Cand &c) { return x < c.a; }) -
                       B.begin());
        };
        for (int ka = 0; ka < int(A.size()); ++ka) {
            const Cand &a = A[ka];
            // Only B shapes inside these aspect windows can keep the parent's evenness at
            // or above the floor (parent evenness <= mnA·shareA / (mxB·shareB), and the
            // mirror), so the pruning is exact.
            int h0 = 0, h1 = int(B.size()), v0 = 0, v1 = int(B.size());
            if (f > 0) {
                h0 = lower(a.a * f * a.mx / maxMnB * (1 - 1e-12));
                h1 = upper(a.a * a.mn / (f * minMxB) * (1 + 1e-12));
                v0 = lower(a.a * f * minMxB / a.mn * (1 - 1e-12));
                v1 = upper(a.a * maxMnB / (f * a.mx) * (1 + 1e-12));
            }
            for (int kb = h0; kb < h1; ++kb) {
                // Side by side: equal heights, widths add (plus one gap).
                const Cand &b = B[kb];
                const double ah = a.a + b.a, fa = a.a / ah;
                Cand c;
                c.a = ah;
                c.beta = a.beta + b.beta + 1.0;
                c.mn = std::min(a.mn * fa, b.mn * (1 - fa));
                c.mx = std::max(a.mx * fa, b.mx * (1 - fa));
                c.lm = std::min(a.lm * fa, b.lm * (1 - fa));
                c.ia = ka;
                c.ib = kb;
                c.split = int16_t(s);
                add(c);
            }
            for (int kb = v0; kb < v1; ++kb) {
                // Stacked: equal widths, heights add (plus one gap).
                const Cand &b = B[kb];
                const double ia = 1 / a.a, ib = 1 / b.a, av = 1 / (ia + ib), fv = ia / (ia + ib);
                Cand c;
                c.a = av;
                c.beta = av * (a.beta * ia + b.beta * ib - 1.0);
                c.mn = std::min(a.mn * fv, b.mn * (1 - fv));
                c.mx = std::max(a.mx * fv, b.mx * (1 - fv));
                c.lm = std::min(a.lm * fv, b.lm * (1 - fv));
                c.ia = ka;
                c.ib = kb;
                c.split = int16_t(s);
                c.vert = 1;
                add(c);
            }
        }
    }
    for (int k : m_used)
        m_table[k] = -1;
    for (int k : m_usedLarge)
        m_tableLarge[k] = -1;
    if (m_p.cap > 0 && delta == m_p.delta) {
        capList(res, delta, false);
        capList(large, delta, true);
    }
    for (Cand &c : large) {
        c.rep = 1;
        const bool dup = std::any_of(res.begin(), res.end(), [&](const Cand &d) {
            return d.a == c.a && d.beta == c.beta && d.mn == c.mn && d.mx == c.mx && d.lm == c.lm;
        });
        if (!dup)
            res.push_back(c);
    }
    std::sort(res.begin(), res.end(), byAspect);
}

// Too many shapes for this run: merge neighbouring buckets (double the bucket width) until
// it fits. That keeps the spread of shapes, unlike keeping only the most even ones.
void Packer::capList(std::vector<Cand> &res, double delta, bool largeKey) const
{
    double d = delta;
    while (int(res.size()) > m_p.cap) {
        d *= 2;
        std::vector<Cand> merged;
        std::vector<int> keys;
        std::sort(res.begin(), res.end(), byAspect);
        for (const Cand &c : res) {
            const int k = bucketOf(c.a, d);
            if (keys.empty() || keys.back() != k) {
                keys.push_back(k);
                merged.push_back(c);
            } else if (largeKey ? betterLarge(c, merged.back()) : better(c, merged.back())) {
                merged.back() = c;
            }
        }
        res.swap(merged);
    }
}

int Packer::pick(double W, double H, double gap, double &best) const
{
    const auto &r = roots();
    int k = -1;
    best = -1;
    for (int i = 0; i < int(r.size()); ++i) {
        if (r[i].rep != 0) // the plain choice is among the most-even representatives only
            continue;
        const double s = score(r[i], W, H, gap);
        if (s > best) { // strict, so the first (lowest aspect) wins ties
            best = s;
            k = i;
        }
    }
    return k;
}

double Packer::score(const Cand &c, double W, double H, double gap) const
{
    const double h = std::min(H, (W - c.beta * gap) / c.a);
    const double w = c.a * h + c.beta * gap;
    if (h <= 0 || w <= 0)
        return 0;
    return (w * h) / (W * H) * std::exp(m_p.evenExp * std::log(c.mn / c.mx));
}

// A total order for a bucket's representative, so the result never depends on scan order
// (and so not on how much was pruned): most even, then smaller aspect, smaller gap term,
// lower split, side by side before stacked.
bool Packer::better(const Cand &x, const Cand &y) const
{
    const double ex = x.mn / x.mx, ey = y.mn / y.mx;
    if (ex != ey)
        return ex > ey;
    if (x.a != y.a)
        return x.a < y.a;
    if (x.beta != y.beta)
        return x.beta < y.beta;
    if (x.split != y.split)
        return x.split < y.split;
    return x.vert < y.vert;
}

bool Packer::betterLarge(const Cand &x, const Cand &y) const
{
    const double e = m_p.largeEven;
    const double kx = std::log(x.lm) + e * std::log(x.mn / x.mx);
    const double ky = std::log(y.lm) + e * std::log(y.mn / y.mx);
    if (kx != ky)
        return kx > ky;
    return better(x, y);
}

void Packer::placeRun(int i, int j, int k, double x, double y, double w, double h, double g,
                      std::vector<Rect> &out) const
{
    const Cand &c = m_cands[idx(i, j)][k];
    if (i == j) {
        out[i] = {x, y, w, h};
        return;
    }
    const int s = c.split;
    const Cand &A = m_cands[idx(i, s)][c.ia];
    if (!c.vert) {
        const double wa = A.a * h + A.beta * g;
        placeRun(i, s, c.ia, x, y, wa, h, g, out);
        placeRun(s + 1, j, c.ib, x + wa + g, y, w - wa - g, h, g, out);
    } else {
        const double ha = (w - A.beta * g) / A.a;
        placeRun(i, s, c.ia, x, y, w, ha, g, out);
        placeRun(s + 1, j, c.ib, x, y + ha + g, w, h - ha - g, g, out);
    }
}

// ---------------------------------------------------------------------------

Params Mosaic::tieBreakParams()
{
    Params p;
    p.evenFloor = 0.25; // the layouts that let Large grow sit just under plain's 0.3
    p.largeReps = true;
    return p;
}

void Mosaic::build(const std::vector<Pane> &panes)
{
    m_panes = panes;
    m_large = std::any_of(panes.begin(), panes.end(), [](const Pane &p) { return p.weight > 1; });
    m_plain.build(panes);
    // A separate build for the alternatives: one build at the lower floor would change
    // plain's own pick through the shape cap.
    if (m_large) {
        std::vector<Pane> normal = panes;
        for (Pane &p : normal)
            p.weight = 1;
        m_normal.build(normal);
        m_alt.build(panes);
    }
}

int Mosaic::runs() const
{
    return m_plain.runs() + (m_large ? m_normal.runs() + m_alt.runs() : 0);
}

double Mosaic::growthOf(const std::vector<Rect> &r, const std::vector<Rect> &normal) const
{
    double g = kInf;
    for (size_t i = 0; i < m_panes.size(); ++i)
        if (m_panes[i].weight > 1)
            g = std::min(g, r[i].w * r[i].h / (normal[i].w * normal[i].h));
    return g;
}

Result Mosaic::layout(double W, double H, double gap, const Tree *held)
{
    Result res;
    if (m_panes.empty())
        return res;
    std::vector<Rect> plain;
    double s0;
    const int b0 = m_plain.layout(W, H, gap, plain, &s0);
    const double black0 = blackOf(plain, W, H);
    res.rects = plain;
    res.tree = treeOf(m_plain.roots()[b0]);
    res.score = s0;
    res.black = black0;
    std::vector<Rect> normal;
    if (m_large) {
        m_normal.layout(W, H, gap, normal);
        res.growth = growthOf(plain, normal);
    }

    // The tie-breaker.
    if (m_large && res.growth < m_r.growth) {
        const auto &roots = m_alt.roots();
        std::vector<Rect> r;
        int best = -1;
        bool meets = false;
        double bestScore = -1, bestGrowth = res.growth;
        for (int k = 0; k < int(roots.size()); ++k) {
            m_alt.place(k, W, H, gap, r);
            if (blackOf(r, W, H) > black0 + m_r.blackCap)
                continue;
            const double g = growthOf(r, normal), s = m_alt.scoreOf(k, W, H, gap);
            if (g >= m_r.growth) {
                if (!meets || s > bestScore) {
                    meets = true;
                    bestScore = s;
                    best = k;
                    bestGrowth = g;
                }
            } else if (!meets && g > bestGrowth) {
                best = k;
                bestGrowth = g;
            }
        }
        if (best >= 0) {
            m_alt.place(best, W, H, gap, res.rects);
            res.tree = treeOf(roots[best]);
            res.score = m_alt.scoreOf(best, W, H, gap);
            res.black = blackOf(res.rects, W, H);
            res.growth = bestGrowth;
        }
    }

    // The resize hold: look the held tree up (plain's roots first) and keep it if it is
    // still good enough.
    if (held && *held != res.tree) {
        const double keep = m_large ? m_r.keepLarge : m_r.keepNormal;
        for (Packer *q : {&m_plain, m_large ? &m_alt : nullptr}) {
            if (!q)
                continue;
            const auto &roots = q->roots();
            const auto it = std::find_if(roots.begin(), roots.end(),
                                         [&](const Cand &c) { return treeOf(c) == *held; });
            if (it == roots.end())
                continue;
            const int k = int(it - roots.begin());
            std::vector<Rect> r;
            q->place(k, W, H, gap, r);
            const double s = q->scoreOf(k, W, H, gap);
            bool ok = s >= res.score * (1 - keep);
            double g = kInf;
            if (m_large) {
                g = growthOf(r, normal);
                ok = ok && blackOf(r, W, H) <= black0 + m_r.holdBlack &&
                     (g >= m_r.holdGrowth || g >= res.growth);
            }
            if (ok) {
                const double black = blackOf(r, W, H);
                res.meetsFresh = !m_large || (black <= black0 + m_r.blackCap &&
                                              g >= std::min(m_r.growth, res.growth));
                res.rects = std::move(r);
                res.tree = *held;
                res.score = s;
                res.black = black;
                res.growth = g;
                res.held = true;
            }
            break;
        }
    }
    return res;
}

double blackOf(const std::vector<Rect> &rects, double W, double H)
{
    double a = 0;
    for (const Rect &r : rects)
        a += r.w * r.h;
    return 100 * (1 - a / (W * H));
}

Tree treeOf(const Cand &c) { return {c.a, c.beta, c.mn, c.mx}; }

std::vector<Rect> snapped(const std::vector<Rect> &rects)
{
    std::vector<Rect> out;
    out.reserve(rects.size());
    for (const Rect &r : rects) {
        const double x0 = std::round(r.x), y0 = std::round(r.y);
        out.push_back({x0, y0, std::round(r.x + r.w) - x0, std::round(r.y + r.h) - y0});
    }
    return out;
}

} // namespace rl::mosaic
