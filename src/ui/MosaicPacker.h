#pragma once

#include <cstdint>
#include <limits>
#include <vector>

// Mosaic packing: every camera picture at its own shape, no cropping, tiled into the box
// with as little black as possible. Pure logic, no Qt objects, so it is tested directly
// and can be built on a worker thread. Design: wayfinder/mosaic-live-view tickets 02, 05,
// 08, 10-12 and the research findings 08 and 11.
//
// The layout is an order-preserving slicing tree over the panes (each subtree is a
// contiguous run of panes, joined side by side or stacked), scored as
//   fill × (min/max of area-per-weight)^0.25
// with weight 1 for a Normal pane and 2 for a Large one. Two phases:
//   build(panes)  box-independent search, run when the panes change (about 1-30 ms)
//   layout(box)   re-scores the few hundred finished layouts, run on every resize (< 1 ms)
// Each sub-layout is affine in the gap (width = a·height + beta·gap), so gaps are exact and
// every pane has exactly its picture's ratio.
namespace rl::mosaic {

struct Pane {
    double ratio = 16.0 / 9; // width / height of the drawn picture
    int weight = 1;          // 1 Normal, 2 Large
};

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
};

struct Params {
    double delta = 0.02;      // bucket width in ln(aspect) for runs
    double rootDelta = 0.002; // bucket width for the whole set
    double evenFloor = 0.3;   // prune sub-layouts less even than this (exact, see layout())
    double evenExp = 0.25;
    int cap = 64;             // at most this many shapes per run; neighbouring buckets merge
    // Also keep, per bucket, the shape that gives the Large panes the biggest share
    // (key ln(large share) + largeEven·ln(evenness)). The tie-breaker's build uses it.
    bool largeReps = false;
    double largeEven = 0.25;
};

// One candidate sub-layout for a run of panes.
struct Cand {
    double a = 1, beta = 0;   // gap-free aspect; gap coefficient
    double mn = 1, mx = 1;    // min / max area-per-weight share
    double lm = std::numeric_limits<double>::infinity(); // min share over Large panes
    int ia = -1, ib = -1;     // child candidates (-1 for a leaf)
    int16_t split = 0;
    uint8_t vert = 0;         // 0 side by side, 1 stacked
    uint8_t rep = 0;          // 0 most even, 1 Large-share representative
};

// The plain best-fit packer.
class Packer
{
public:
    explicit Packer(Params p = {}) : m_p(p) {}

    void build(const std::vector<Pane> &panes);
    // Places the best root layout, centred, in pane order. Returns its index in roots().
    // If the winner scores below floor^0.25 the search reruns with a lower floor, so the
    // result is the same as an unpruned search (that rerun is rare and counts in runs()).
    int layout(double W, double H, double gap, std::vector<Rect> &out, double *score = nullptr);

    int size() const { return m_n; }
    const std::vector<Cand> &roots() const { return m_cands[idx(0, m_n - 1)]; }
    double scoreOf(int root, double W, double H, double gap) const;
    void place(int root, double W, double H, double gap, std::vector<Rect> &out) const;
    // The panes in an in-order walk of a root layout's stored tree.
    std::vector<int> leaves(int root) const;
    int runs() const { return m_runs; } // searches done, including floor reruns
    double floorUsed() const { return m_floor; }

private:
    int idx(int i, int j) const { return i * m_n + j; }
    void run(double floor);
    void combine(int i, int j, double delta);
    void capList(std::vector<Cand> &res, double delta, bool largeKey) const;
    int pick(double W, double H, double gap, double &best) const;
    double score(const Cand &c, double W, double H, double gap) const;
    bool better(const Cand &x, const Cand &y) const;
    bool betterLarge(const Cand &x, const Cand &y) const;
    void walk(int i, int j, int k, std::vector<int> &leaves) const;
    void placeRun(int i, int j, int k, double x, double y, double w, double h, double g,
                  std::vector<Rect> &out) const;

    Params m_p;
    std::vector<Pane> m_panes;
    int m_n = 0;
    double m_floor = 0;
    int m_runs = 0;
    std::vector<std::vector<Cand>> m_cands;
    std::vector<int> m_table, m_tableLarge, m_used, m_usedLarge;
};

// Which layout is on screen, independent of where it sits in a root list.
struct Tree {
    double a = 0, beta = 0, mn = 0, mx = 0;
    bool operator==(const Tree &) const = default;
};

struct Result {
    std::vector<Rect> rects; // in pane order
    Tree tree;
    double score = 0;
    double black = 0;        // percent of the box, gaps included
    // Smallest Large pane area ÷ its area with every pane Normal; infinity without Large.
    double growth = std::numeric_limits<double>::infinity();
    bool held = false;       // the held tree was kept
    // False when a held layout falls short of the rule a fresh one meets (Large panes grown
    // as much as fresh, up to `growth`, within `blackCap`): it should be re-packed once the
    // resize settles. Always true for fresh layouts and without Large.
    bool meetsFresh = true;
};

struct Rules {
    double growth = 1.3;       // a fresh layout grows every Large pane at least this much...
    double blackCap = 1.0;     // ...for at most this many points more black than plain
    double holdGrowth = 1.0;   // while resizing, a held layout must keep this growth...
    double holdBlack = 2.0;    // ...and stay within this many points of plain's black
    double keepLarge = 0.05;   // held within 5% of the fresh score (mosaic with Large)
    double keepNormal = 0.02;  // held within 2% (no Large)
};

// The mosaic as the app uses it: the plain packer, plus the Large tie-breaker (ticket 11,
// heavier variant per ticket 12) and the resize hold.
//   - No Large, or plain already grows every Large pane ≥ 1.3×: plain's layout, unchanged.
//   - Otherwise, from a second build at evenness floor 0.25 with Large-share
//     representatives: the best-scoring layout within +1 point of plain's black that grows
//     every Large pane ≥ 1.3×; failing that, the one with the most growth.
//   - Given the held tree (the one on screen while the window is resized), keep it if it
//     scores within keepLarge / keepNormal of the fresh choice and, with Large panes, stays
//     within holdBlack of plain and grows ≥ holdGrowth (or as much as the fresh choice).
//     holdGrowth is 1.0: any gate above it flips back and forth as growth hovers at it
//     (ticket 18). Result::meetsFresh says when a held layout should be re-packed.
// Fresh layouts (no held tree) are a pure function of panes and box.
class Mosaic
{
public:
    explicit Mosaic(Rules r = {}) : m_r(r) {}

    void build(const std::vector<Pane> &panes);
    Result layout(double W, double H, double gap, const Tree *held = nullptr);

    int size() const { return int(m_panes.size()); }
    bool hasLarge() const { return m_large; }
    int runs() const;

    static Params plainParams() { return {}; }
    static Params tieBreakParams();

private:
    double growthOf(const std::vector<Rect> &r, const std::vector<Rect> &normal) const;

    Rules m_r;
    std::vector<Pane> m_panes;
    bool m_large = false;
    Packer m_plain{plainParams()};
    Packer m_normal{plainParams()}; // every pane Normal: the reference for growth
    Packer m_alt{tieBreakParams()};
};

// Black share of the box in percent: everything not covered by a pane, gaps included.
double blackOf(const std::vector<Rect> &rects, double W, double H);
Tree treeOf(const Cand &c);
// Rounds each edge to whole pixels (not each width), so neighbours keep exact gaps.
std::vector<Rect> snapped(const std::vector<Rect> &rects);

} // namespace rl::mosaic
