// A simulated options book for full-revaluation scenario analysis, plus a
// delta-gamma-vega Taylor approximation to compare full revaluation against.
// Reuses black_scholes.hpp (the same closed form already validated by
// test_main.cpp and bench_bs_grid) as the pricer for both the "fast" scenario
// path and the position-level Greeks; a genuinely independent quadrature
// pricer below serves as the reference oracle for the fast path.
#pragma once
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "black_scholes.hpp"

namespace scenario {

struct Position {
    int underlying_id;
    double S0;     // spot at book date
    double K;      // strike
    double r;      // risk-free rate
    double sigma0; // vol at book date
    double T;      // time to maturity, years (held fixed across the scenario set;
                   // this is a market-risk scenario grid, not a 1-day time-decay roll)
    bool is_call;
    double qty; // signed contract count; negative is short (written) options
};

struct PositionGreeks {
    double price;
    double delta;
    double gamma;
    double vega;
};

inline PositionGreeks position_base(const Position& p) {
    bs::BSParams bp{p.S0, p.K, p.r, p.sigma0, p.T};
    PositionGreeks g{};
    if (p.is_call) {
        g.price = bs::call_price(bp);
        bs::Greeks gg = bs::call_greeks(bp);
        g.delta = gg.delta;
        g.gamma = gg.gamma;
        g.vega = gg.vega;
    } else {
        g.price = bs::put_price(bp);
        bs::Greeks gg = bs::put_greeks(bp);
        g.delta = gg.delta;
        g.gamma = gg.gamma;
        g.vega = gg.vega;
    }
    return g;
}

// Full revaluation under a parallel spot shock (fraction of S0, same for
// every underlying: a single systematic market factor, not a per-name
// factor model; see README for the tradeoff) and a parallel vol shift
// (vol points, added to each name's own sigma0).
inline double full_reval_price(const Position& p, double spot_shock, double vol_shift) {
    double S = p.S0 * (1.0 + spot_shock);
    double sigma = p.sigma0 + vol_shift;
    if (sigma < 0.02) sigma = 0.02; // floor: vol cannot go to zero or negative
    bs::BSParams bp{S, p.K, p.r, sigma, p.T};
    return p.is_call ? bs::call_price(bp) : bs::put_price(bp);
}

// Independent reference oracle: prices the same contract by direct
// trapezoidal quadrature of the risk-neutral lognormal payoff expectation,
// not via the closed-form erfc formula full_reval_price uses. Deliberately
// slow (20001-point grid) and deliberately a different algorithm, so
// agreement with full_reval_price is evidence the closed-form path is
// correct, not a tautology.
inline double full_reval_price_oracle(const Position& p, double spot_shock, double vol_shift) {
    double S = p.S0 * (1.0 + spot_shock);
    double sigma = p.sigma0 + vol_shift;
    if (sigma < 0.02) sigma = 0.02;
    double r = p.r, T = p.T, K = p.K;
    double drift = (r - 0.5 * sigma * sigma) * T;
    double diffusion = sigma * std::sqrt(T);

    const int N = 20000;
    const double zlo = -8.0, zhi = 8.0;
    const double h = (zhi - zlo) / N;
    double sum = 0.0;
    auto integrand = [&](double z) {
        double ST = S * std::exp(drift + diffusion * z);
        double payoff = p.is_call ? std::max(ST - K, 0.0) : std::max(K - ST, 0.0);
        double phi = 0.3989422804014327 * std::exp(-0.5 * z * z);
        return payoff * phi;
    };
    // Composite trapezoidal rule.
    double prev = integrand(zlo);
    for (int i = 1; i <= N; ++i) {
        double z = zlo + i * h;
        double cur = integrand(z);
        sum += 0.5 * (prev + cur) * h;
        prev = cur;
    }
    return std::exp(-r * T) * sum;
}

// Position-level Taylor (delta-gamma-vega) approximation to the P&L under
// the same shock, using Greeks measured at the book date. No cross term
// between spot and vol (that is exactly the simplification this project
// measures the cost of) and no theta (the scenario set holds time fixed).
inline double greeks_approx_pnl(const Position& p, const PositionGreeks& base, double spot_shock,
                                 double vol_shift) {
    double dS = p.S0 * spot_shock;
    double dSigma = vol_shift;
    double dPrice = base.delta * dS + 0.5 * base.gamma * dS * dS + base.vega * dSigma;
    return p.qty * dPrice;
}

inline double full_reval_pnl(const Position& p, double base_price, double spot_shock,
                              double vol_shift) {
    return p.qty * (full_reval_price(p, spot_shock, vol_shift) - base_price);
}

// 1,800 positions over 60 underlyings: a net-short-optionality, put-heavy
// premium-selling book (70% written puts struck just out of the money,
// 30% written calls struck further out of the money). This shape is what
// gives the Greeks shortcut something real to get wrong: an OTM written
// put's gamma and vega both rise as the underlying falls toward the
// strike, which a Taylor expansion anchored at the book-date Greeks cannot
// see coming, so a moderate-to-large down move understates the true loss;
// the calls are struck far enough out of the money that a comparable up
// move does not create the same effect, which is what breaks the
// down-versus-up symmetry a flat short-gamma book would otherwise have.
inline std::vector<Position> generate_book(uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> put_moneyness(0.75, 0.95);  // K/S0, OTM puts
    std::uniform_real_distribution<double> call_moneyness(1.05, 1.25); // K/S0, OTM calls
    std::uniform_real_distribution<double> maturity(1.0 / 12.0, 1.0);
    std::uniform_real_distribution<double> vol0(0.15, 0.45);
    std::uniform_real_distribution<double> spot0(20.0, 500.0);
    std::uniform_real_distribution<double> rate(0.02, 0.05);
    std::uniform_real_distribution<double> qty_mag(10.0, 200.0);
    std::bernoulli_distribution is_put_dist(0.70); // 70% puts, 30% calls

    const int n_underlyings = 60;
    const int pos_per_name = 30; // 60 * 30 = 1800

    std::vector<double> u_spot(n_underlyings), u_vol(n_underlyings);
    for (int u = 0; u < n_underlyings; ++u) {
        u_spot[u] = spot0(rng);
        u_vol[u] = vol0(rng);
    }

    std::vector<Position> book;
    book.reserve(n_underlyings * pos_per_name);
    for (int u = 0; u < n_underlyings; ++u) {
        for (int i = 0; i < pos_per_name; ++i) {
            Position p{};
            p.underlying_id = u;
            p.S0 = u_spot[u];
            p.r = rate(rng);
            p.sigma0 = u_vol[u];
            p.T = maturity(rng);
            bool is_put = is_put_dist(rng);
            p.is_call = !is_put;
            p.K = p.S0 * (is_put ? put_moneyness(rng) : call_moneyness(rng));
            p.qty = -qty_mag(rng); // the whole book is written (short) premium
            book.push_back(p);
        }
    }
    return book;
}

} // namespace scenario
