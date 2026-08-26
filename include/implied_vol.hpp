// Implied volatility solver: bisection root-find on the closed-form
// Black-Scholes price in black_scholes.hpp. Independent of the SVI fit and
// of the screener; it is the one piece both of those build on.
//
// This is the C++ half of a two-language, two-algorithm cross-check:
// python/implied_vol_ref.py inverts the same prices with
// scipy.optimize.brentq (Brent, not bisection) applied to
// black_scholes_ref.py (a different Black-Scholes implementation, in a
// different language). See docs/python_svi_cross_check_output.txt for the
// measured agreement between the two.
#pragma once
#include <cmath>
#include <limits>

#include "black_scholes.hpp"
#include "mc_engine.hpp" // for mc::OptionType

namespace ivol {

struct ImpliedVolResult {
    double vol;
    int iterations;
    bool converged;
};

// Bisection on sigma in [lo, hi]. BS price is strictly increasing in sigma
// for sigma > 0 (vega > 0 everywhere in that range), so the bracket has at
// most one root and bisection cannot fail to converge given a bracket wide
// enough to contain it. If `price` is not attainable by any sigma in
// [lo, hi] (below intrinsic, or above what hi implies), converged is false
// rather than silently extrapolating.
inline ImpliedVolResult solve(mc::OptionType type, double price, double S, double K, double r,
                               double T, double lo = 1e-4, double hi = 5.0, double tol = 1e-8,
                               int max_iter = 200) {
    auto price_at = [&](double sigma) {
        bs::BSParams p{S, K, r, sigma, T};
        return (type == mc::OptionType::Call) ? bs::call_price(p) : bs::put_price(p);
    };
    double f_lo = price_at(lo) - price;
    double f_hi = price_at(hi) - price;
    if (f_lo > 0.0 || f_hi < 0.0) {
        return {std::numeric_limits<double>::quiet_NaN(), 0, false};
    }
    int it = 0;
    double mid = 0.5 * (lo + hi);
    for (; it < max_iter; ++it) {
        mid = 0.5 * (lo + hi);
        double f_mid = price_at(mid) - price;
        if (std::fabs(f_mid) < tol || (hi - lo) < tol) {
            return {mid, it + 1, true};
        }
        if (f_mid > 0.0) hi = mid; else lo = mid;
    }
    return {mid, it, true};
}

} // namespace ivol
