// Raw SVI (Gatheral) per-expiry total-variance parameterization, fit with a
// constrained Nelder-Mead simplex search.
//
// Hard constraint mechanism: every candidate parameter vector the simplex
// evaluates is passed through is_convex() (butterfly / no static arbitrage:
// total variance convex in log-moneyness) and, for every expiry after the
// first, respects_calendar_floor() (total variance at this expiry must sit
// at or above the previous, shorter expiry's fitted total variance at every
// point on a shared grid). A candidate that fails either check is given an
// objective of +infinity. Infinity can win a simplex step only by being the
// least-bad vertex temporarily kept around for the reflection/contraction
// machinery; it can never become the answer, because fit_slice() separately
// tracks the best FEASIBLE point seen at every evaluation (see `best` /
// `best_val` below) and returns that, not whatever the simplex last held.
// That is what makes this "enforced during the fit" rather than "checked
// after": the returned slice is feasible by construction, not by luck.
//
// Contrast: model-validation-alerting (github.com/Manas103/model-validation-
// alerting) runs no-arbitrage checks as a post-hoc pass over an already-
// fitted synthetic surface. Here the same two conditions gate every
// candidate the optimizer is allowed to keep. See README, "Sibling
// comparison".
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace svi {

struct Params {
    double a, b, rho, m, sigma;
};

// Raw SVI total variance at log-moneyness k: w(k) = a + b*(rho*(k-m) +
// sqrt((k-m)^2 + sigma^2)).
inline double total_variance(const Params& p, double k) {
    double x = k - p.m;
    return p.a + p.b * (p.rho * x + std::sqrt(x * x + p.sigma * p.sigma));
}

// Discretized butterfly / no-static-arbitrage check: total variance must be
// non-negative and convex in log-moneyness. Central second difference on
// `grid` (assumed uniformly spaced) must be >= -eps everywhere; a genuinely
// convex analytic function passes this for any eps >= 0 as the grid is
// refined, so eps is a numerical-noise floor, not a tolerance for real
// violations.
inline bool is_convex(const Params& p, const std::vector<double>& grid, double eps = 1e-9) {
    if (grid.size() >= 2) {
        double dk = grid[1] - grid[0];
        if (grid.size() >= 3) {
            for (size_t i = 1; i + 1 < grid.size(); ++i) {
                double w0 = total_variance(p, grid[i - 1]);
                double w1 = total_variance(p, grid[i]);
                double w2 = total_variance(p, grid[i + 1]);
                double d2 = (w0 - 2.0 * w1 + w2) / (dk * dk);
                if (d2 < -eps) return false;
            }
        }
    }
    for (double k : grid) {
        if (total_variance(p, k) < -eps) return false;
    }
    return true;
}

// Calendar no-arbitrage: total variance at this expiry must be >= a floor
// (the previous, shorter expiry's fitted total variance) at every point on
// `grid`, i.e. total variance is non-decreasing across expiries at fixed
// log-moneyness.
inline bool respects_calendar_floor(const Params& p, const std::vector<double>& grid,
                                     const std::vector<double>& floor_w, double eps = 1e-9) {
    for (size_t i = 0; i < grid.size(); ++i) {
        if (total_variance(p, grid[i]) < floor_w[i] - eps) return false;
    }
    return true;
}

struct FitPoint {
    double k, w, weight;
};

struct FitResult {
    Params params;
    double sse;      // weighted sum of squared error on the fit points, if feasible
    bool feasible;    // true iff params satisfies both hard constraints on the given grids
    int iterations;
};

namespace detail {

using Vec5 = std::array<double, 5>;

inline Params to_params(const Vec5& v) { return {v[0], v[1], v[2], v[3], v[4]}; }
inline Vec5 from_params(const Params& p) { return {p.a, p.b, p.rho, p.m, p.sigma}; }

inline double objective(const Vec5& v, const std::vector<FitPoint>& pts,
                         const std::vector<double>& conv_grid,
                         const std::vector<double>* floor_grid, const std::vector<double>* floor_w) {
    Params p = to_params(v);
    // Cheap parameter-box rejects before the more expensive grid checks:
    // these are necessary conditions for a sane SVI slice (b >= 0, |rho| <
    // 1, sigma > 0), not the arbitrage conditions themselves.
    //
    // sigma <= 1.0 and |m| <= 0.6 are a second, deliberate pair of box
    // bounds, tied to the log-moneyness domain actually being fit
    // (roughly [-0.4, 0.35] here), not no-arbitrage conditions. Two
    // successive unbounded fits found feasible-but-degenerate slices: first
    // sigma up to ~9.5 (a domain of width ~0.75), then, after capping sigma
    // alone at 3.0, sigma pegged at that new cap AND m drifted to ~1.2-1.7,
    // outside the data range entirely. Both are the same underlying raw-SVI
    // non-identifiability: (b, m, sigma) can jointly move the sqrt term's
    // "kink" arbitrarily far from the data and flatten it into a near-affine
    // function over the fitted range, letting a, b absorb curvature that
    // should live in sigma, m. Bounding sigma and m to the same order as
    // the domain forces the fit to actually use SVI's shape rather than
    // degenerate toward a generic smooth curve that happens to satisfy the
    // two hard no-arbitrage constraints. See README, Findings.
    if (p.b < 0.0 || p.sigma <= 1e-6 || p.sigma > 1.0 || p.rho <= -0.999 || p.rho >= 0.999 ||
        p.m < -0.6 || p.m > 0.6) {
        return std::numeric_limits<double>::infinity();
    }
    if (!is_convex(p, conv_grid)) return std::numeric_limits<double>::infinity();
    if (floor_grid && !respects_calendar_floor(p, *floor_grid, *floor_w)) {
        return std::numeric_limits<double>::infinity();
    }
    double sse = 0.0;
    for (const auto& pt : pts) {
        double diff = total_variance(p, pt.k) - pt.w;
        sse += pt.weight * diff * diff;
    }
    return sse;
}

} // namespace detail

// Constrained Nelder-Mead fit of a single SVI slice.
//
// `conv_grid` is the log-moneyness grid used for this slice's discretized
// butterfly check. `floor_grid`/`floor_w` (both nullptr for the first,
// shortest expiry) carry the previous expiry's fitted total variance on a
// shared grid, used as the calendar no-arbitrage floor for this slice.
inline FitResult fit_slice(const std::vector<FitPoint>& pts, const std::vector<double>& conv_grid,
                            const Params& init, const std::vector<double>* floor_grid = nullptr,
                            const std::vector<double>* floor_w = nullptr, int max_iter = 6000) {
    using detail::Vec5;
    auto obj = [&](const Vec5& v) {
        return detail::objective(v, pts, conv_grid, floor_grid, floor_w);
    };

    Vec5 x0 = detail::from_params(init);
    Vec5 best = x0;
    double best_val = obj(x0);

    // If the caller's initial guess is itself infeasible, raise the level
    // (a) until it is not: raising a moves total variance up uniformly
    // without changing its shape, which clears both a negative-total-
    // variance violation and any finite calendar floor without touching
    // sigma (widening sigma is what caused the degenerate large-sigma fits
    // described above, so the guard must not do that). If sigma itself is
    // out of the valid box, snap it to a sane default first. This only
    // seeds the simplex; it does not weaken the check applied to every
    // subsequent candidate.
    if (x0[4] <= 1e-6 || x0[4] > 1.0) x0[4] = 0.15;
    if (x0[1] < 0.0) x0[1] = 0.05;
    if (x0[3] < -0.6 || x0[3] > 0.6) x0[3] = 0.0;
    int guard = 0;
    while (!std::isfinite(best_val) && guard < 400) {
        x0[0] += 0.005 * static_cast<double>(guard + 1); // a, growing step
        best = x0;
        best_val = obj(x0);
        ++guard;
    }

    std::array<Vec5, 6> simplex;
    std::array<double, 6> fval;
    simplex[0] = x0;
    const double step[5] = {0.01, 0.02, 0.05, 0.02, 0.02};
    for (int i = 0; i < 5; ++i) {
        simplex[i + 1] = x0;
        simplex[i + 1][static_cast<size_t>(i)] += step[static_cast<size_t>(i)];
    }
    for (int i = 0; i < 6; ++i) {
        fval[static_cast<size_t>(i)] = obj(simplex[static_cast<size_t>(i)]);
        if (fval[static_cast<size_t>(i)] < best_val) {
            best_val = fval[static_cast<size_t>(i)];
            best = simplex[static_cast<size_t>(i)];
        }
    }

    const double alpha = 1.0, gamma = 2.0, rho_c = 0.5, sigma_c = 0.5;
    int iter = 0;
    for (; iter < max_iter; ++iter) {
        std::array<int, 6> ord = {0, 1, 2, 3, 4, 5};
        std::sort(ord.begin(), ord.end(), [&](int i1, int i2) {
            return fval[static_cast<size_t>(i1)] < fval[static_cast<size_t>(i2)];
        });
        std::array<Vec5, 6> s2;
        std::array<double, 6> f2;
        for (int i = 0; i < 6; ++i) {
            s2[static_cast<size_t>(i)] = simplex[static_cast<size_t>(ord[static_cast<size_t>(i)])];
            f2[static_cast<size_t>(i)] = fval[static_cast<size_t>(ord[static_cast<size_t>(i)])];
        }
        simplex = s2;
        fval = f2;
        if (fval[0] < best_val) { best_val = fval[0]; best = simplex[0]; }

        double spread = fval[5] - fval[0];
        if (std::isfinite(spread) && spread < 1e-15) break;

        Vec5 centroid{};
        for (int i = 0; i < 5; ++i) {
            for (int d = 0; d < 5; ++d) centroid[static_cast<size_t>(d)] += simplex[static_cast<size_t>(i)][static_cast<size_t>(d)];
        }
        for (int d = 0; d < 5; ++d) centroid[static_cast<size_t>(d)] /= 5.0;

        Vec5 xr{};
        for (int d = 0; d < 5; ++d) {
            xr[static_cast<size_t>(d)] = centroid[static_cast<size_t>(d)] +
                alpha * (centroid[static_cast<size_t>(d)] - simplex[5][static_cast<size_t>(d)]);
        }
        double fr = obj(xr);

        if (fr < fval[0]) {
            Vec5 xe{};
            for (int d = 0; d < 5; ++d) {
                xe[static_cast<size_t>(d)] = centroid[static_cast<size_t>(d)] +
                    gamma * (xr[static_cast<size_t>(d)] - centroid[static_cast<size_t>(d)]);
            }
            double fe = obj(xe);
            if (fe < fr) { simplex[5] = xe; fval[5] = fe; } else { simplex[5] = xr; fval[5] = fr; }
        } else if (fr < fval[4]) {
            simplex[5] = xr;
            fval[5] = fr;
        } else {
            Vec5 xc{};
            for (int d = 0; d < 5; ++d) {
                xc[static_cast<size_t>(d)] = centroid[static_cast<size_t>(d)] +
                    rho_c * (simplex[5][static_cast<size_t>(d)] - centroid[static_cast<size_t>(d)]);
            }
            double fc = obj(xc);
            if (fc < fval[5]) {
                simplex[5] = xc;
                fval[5] = fc;
            } else {
                for (int i = 1; i < 6; ++i) {
                    for (int d = 0; d < 5; ++d) {
                        simplex[static_cast<size_t>(i)][static_cast<size_t>(d)] =
                            simplex[0][static_cast<size_t>(d)] +
                            sigma_c * (simplex[static_cast<size_t>(i)][static_cast<size_t>(d)] - simplex[0][static_cast<size_t>(d)]);
                    }
                    fval[static_cast<size_t>(i)] = obj(simplex[static_cast<size_t>(i)]);
                    if (fval[static_cast<size_t>(i)] < best_val) {
                        best_val = fval[static_cast<size_t>(i)];
                        best = simplex[static_cast<size_t>(i)];
                    }
                }
            }
        }
        if (fval[5] < best_val) { best_val = fval[5]; best = simplex[5]; }
    }

    Params bp = detail::to_params(best);
    bool feasible = std::isfinite(best_val) && is_convex(bp, conv_grid) &&
                     (!floor_grid || respects_calendar_floor(bp, *floor_grid, *floor_w));
    return {bp, feasible ? best_val : std::numeric_limits<double>::infinity(), feasible, iter};
}

} // namespace svi
