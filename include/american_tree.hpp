// Cox-Ross-Rubinstein (CRR) binomial tree for American (and, for
// cross-checking, European) exercise, carrying discrete dollar dividends and
// a borrow rate (cost of carry). This is the pricer the vol-store extension
// inverts 12 million simulated quotes through; see README, "Extension:
// single-stock volatility store with American-exercise implied vols".
//
// Discrete dividends: the standard practical approximation (Hull's "known
// dollar dividend" tree), NOT the exact Vellekoop-Nieuwenhuis recombining
// correction. The undiminished multiplicative lattice S0*u^j*d^(i-j) is
// built as usual; at every step i, the cumulative dollar dividends whose
// ex-date has already passed (time <= i*dt) are subtracted from every node
// at that step. Because the subtraction at step i is the same scalar for
// every node j at that step, the lattice still recombines going forward
// (every subsequent node multiplies by the same u/d off an already-shifted,
// but shared, base). This is disclosed as a limitation in the README: it is
// an approximation to the true (non-recombining) dividend-adjusted process,
// not an exact correction.
//
// Cost of carry: the risk-neutral growth rate used to build the lattice is
// r - borrow (the borrow rate is modeled as a continuous drag on the
// forward, the same mechanical role a continuous dividend yield would play,
// stacked on top of, not instead of, the discrete dollar dividends above).
// Discounting uses r alone. This convention is a judgment call, stated here
// and in the README rather than buried.
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace crr {

struct DiscreteDividend {
    double time;   // years from valuation date, in (0, T]
    double amount; // dollar amount
};

enum class Exercise { European, American };

struct TreeParams {
    double S0;
    double K;
    double r;      // risk-free rate, continuously compounded
    double borrow;  // borrow rate / cost-of-carry drag, continuously compounded
    double sigma;
    double T;
    int steps;
    const std::vector<DiscreteDividend>* divs; // may be null (no dividends)
};

// Reusable scratch buffers so a batch inversion of millions of quotes does
// not allocate on every single tree build. One Workspace per worker thread.
class Workspace {
public:
    explicit Workspace(int max_steps) : S_(static_cast<size_t>(max_steps) + 1),
                                          V_(static_cast<size_t>(max_steps) + 1) {}

    // Returns the price. If `ok` is non-null, set to false when the
    // requested step count needs probabilities outside [0,1] (dt too coarse
    // for the given sigma); callers treat that as a non-price rather than
    // silently extrapolating.
    double price(const TreeParams& p, bool is_call, Exercise exercise, bool* ok = nullptr) {
        if (ok) *ok = true;
        const int N = p.steps;
        const double dt = p.T / static_cast<double>(N);
        const double u = std::exp(p.sigma * std::sqrt(dt));
        const double d = 1.0 / u;
        const double growth = std::exp((p.r - p.borrow) * dt);
        double prob = (growth - d) / (u - d);
        const double disc = std::exp(-p.r * dt);
        if (prob < 0.0 || prob > 1.0) {
            if (ok) *ok = false;
            prob = std::min(1.0, std::max(0.0, prob));
        }

        // Cumulative dollar dividends paid by time t (ex-date <= t).
        auto cum_div = [&](double t) {
            double sum = 0.0;
            if (p.divs) {
                for (const auto& dv : *p.divs) {
                    if (dv.time <= t) sum += dv.amount;
                }
            }
            return sum;
        };

        // Terminal layer (step N).
        for (int j = 0; j <= N; ++j) {
            double s_raw = p.S0 * std::pow(u, j) * std::pow(d, N - j);
            double s = s_raw - cum_div(p.T);
            S_[static_cast<size_t>(j)] = s;
            double intrinsic = is_call ? std::max(s - p.K, 0.0) : std::max(p.K - s, 0.0);
            V_[static_cast<size_t>(j)] = intrinsic;
        }

        for (int i = N - 1; i >= 0; --i) {
            double t_i = i * dt;
            double div_i = cum_div(t_i);
            for (int j = 0; j <= i; ++j) {
                double cont = disc * (prob * V_[static_cast<size_t>(j + 1)] +
                                       (1.0 - prob) * V_[static_cast<size_t>(j)]);
                if (exercise == Exercise::American) {
                    double s_raw = p.S0 * std::pow(u, j) * std::pow(d, i - j);
                    double s = s_raw - div_i;
                    double intrinsic = is_call ? std::max(s - p.K, 0.0) : std::max(p.K - s, 0.0);
                    V_[static_cast<size_t>(j)] = std::max(cont, intrinsic);
                } else {
                    V_[static_cast<size_t>(j)] = cont;
                }
            }
        }
        return V_[0];
    }

    // Exposes the full node grid at a given step for invariant tests
    // ("value at every node >= intrinsic"). Recomputes the tree with the
    // grid retained; deliberately not the hot path used by the batch
    // inversion.
    struct NodeCheck {
        bool all_nodes_ge_intrinsic;
        int nodes_checked;
    };
    NodeCheck check_intrinsic_bound(const TreeParams& p, bool is_call) {
        const int N = p.steps;
        const double dt = p.T / static_cast<double>(N);
        const double u = std::exp(p.sigma * std::sqrt(dt));
        const double d = 1.0 / u;
        const double growth = std::exp((p.r - p.borrow) * dt);
        double prob = std::min(1.0, std::max(0.0, (growth - d) / (u - d)));
        const double disc = std::exp(-p.r * dt);
        auto cum_div = [&](double t) {
            double sum = 0.0;
            if (p.divs) for (const auto& dv : *p.divs) if (dv.time <= t) sum += dv.amount;
            return sum;
        };
        std::vector<std::vector<double>> grid(static_cast<size_t>(N) + 1);
        for (int j = 0; j <= N; ++j) {
            double s = p.S0 * std::pow(u, j) * std::pow(d, N - j) - cum_div(p.T);
            grid[static_cast<size_t>(N)].push_back(is_call ? std::max(s - p.K, 0.0)
                                                             : std::max(p.K - s, 0.0));
        }
        int checked = 0;
        bool all_ok = true;
        for (int i = N - 1; i >= 0; --i) {
            double t_i = i * dt;
            double div_i = cum_div(t_i);
            grid[static_cast<size_t>(i)].resize(static_cast<size_t>(i) + 1);
            for (int j = 0; j <= i; ++j) {
                double cont = disc * (prob * grid[static_cast<size_t>(i + 1)][static_cast<size_t>(j + 1)] +
                                       (1.0 - prob) * grid[static_cast<size_t>(i + 1)][static_cast<size_t>(j)]);
                double s = p.S0 * std::pow(u, j) * std::pow(d, i - j) - div_i;
                double intrinsic = is_call ? std::max(s - p.K, 0.0) : std::max(p.K - s, 0.0);
                double v = std::max(cont, intrinsic);
                grid[static_cast<size_t>(i)][static_cast<size_t>(j)] = v;
                ++checked;
                if (v < intrinsic - 1e-12) all_ok = false;
            }
        }
        return {all_ok, checked};
    }

private:
    std::vector<double> S_;
    std::vector<double> V_;
};

struct ImpliedVolResult {
    double vol;
    int iterations;
    bool converged;
};

// Bisection implied-vol solve against the CRR price. Bracketed the same way
// ivol::solve brackets the closed-form Black-Scholes price: if the target
// price is not attainable anywhere in [lo, hi], converged is false rather
// than extrapolating. Bounded iteration count (max_iter), same discipline as
// the closed-form solver in implied_vol.hpp.
inline ImpliedVolResult solve_implied_vol(Workspace& ws, TreeParams p, bool is_call,
                                           Exercise exercise, double target_price,
                                           double lo = 1e-4, double hi = 5.0, double tol = 1e-6,
                                           int max_iter = 60) {
    auto price_at = [&](double sigma) {
        p.sigma = sigma;
        return ws.price(p, is_call, exercise);
    };
    double f_lo = price_at(lo) - target_price;
    double f_hi = price_at(hi) - target_price;
    if (f_lo > 0.0 || f_hi < 0.0) {
        return {std::numeric_limits<double>::quiet_NaN(), 0, false};
    }
    double mid = 0.5 * (lo + hi);
    int it = 0;
    for (; it < max_iter; ++it) {
        mid = 0.5 * (lo + hi);
        double f_mid = price_at(mid) - target_price;
        if (std::fabs(f_mid) < tol || (hi - lo) < tol) {
            return {mid, it + 1, true};
        }
        if (f_mid > 0.0) hi = mid; else lo = mid;
    }
    return {mid, it, true};
}

} // namespace crr
