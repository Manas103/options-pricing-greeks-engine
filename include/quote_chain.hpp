// A fixed option chain for the tick-driven quote path: one underlying,
// NUM_EXPIRIES maturities x NUM_STRIKES strikes, each strike/expiry pair
// quoted as a call and a put (NUM_CONTRACTS = 2 x NUM_EXPIRIES x
// NUM_STRIKES). Strikes and expiries are fixed at chain-build time; only
// spot and a flat vol shock change per tick. Each pair's expiry-dependent
// pieces (sqrt(T), the discount factor K*exp(-r*T)) are precomputed once
// here rather than recomputed on every tick; see README "Findings" for
// what that bought and what it did not.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace quote {

constexpr int NUM_EXPIRIES = 10;
constexpr int NUM_STRIKES = 26;
constexpr int NUM_PAIRS = NUM_EXPIRIES * NUM_STRIKES;   // 260 strike/expiry pairs
constexpr int NUM_CONTRACTS = NUM_PAIRS * 2;             // 520, a call and a put per pair

// One strike/expiry pair. A call and a put share d1, d2 and both cdf
// evaluations; see quote_engine.hpp for why they are repriced together
// rather than as two independent contracts.
struct Pair {
    double K;
    double T;
    double sqrtT;  // precomputed, fixed
    double discK;  // K * exp(-r*T), precomputed with the chain's flat r, fixed
};

struct ChainSpec {
    double S0 = 100.0;        // reference spot the strike ladder is built around
    double r = 0.03;          // flat risk-free rate
    double base_sigma = 0.20; // base flat vol the feed shocks around
};

inline std::vector<Pair> build_chain(const ChainSpec& spec = ChainSpec{}) {
    std::vector<Pair> chain;
    chain.reserve(NUM_PAIRS);
    for (int e = 0; e < NUM_EXPIRIES; ++e) {
        double frac_t = (NUM_EXPIRIES == 1) ? 0.0 : static_cast<double>(e) / (NUM_EXPIRIES - 1);
        double T = 0.0833 + (2.0 - 0.0833) * frac_t; // 1 month to 2 years
        double sqrtT = std::sqrt(T);
        for (int k = 0; k < NUM_STRIKES; ++k) {
            double frac_k = (NUM_STRIKES == 1) ? 0.0 : static_cast<double>(k) / (NUM_STRIKES - 1);
            double K = spec.S0 * (0.80 + 0.40 * frac_k); // 80% to 120% moneyness
            double discK = K * std::exp(-spec.r * T);
            chain.push_back(Pair{K, T, sqrtT, discK});
        }
    }
    return chain;
}

} // namespace quote
