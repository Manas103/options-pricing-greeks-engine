// Tick-driven quote path: reprices the whole chain (quote_chain.hpp) from a
// single spot/vol tick. Prices through the same closed-form Black-Scholes
// formula already validated in this repository (include/black_scholes.hpp,
// docs/bs_grid_output.txt: 0.02444% max relative error against the
// variance-reduced Monte Carlo engine, the correctness oracle this
// extension carries forward rather than re-measures), not a separate
// approximation, so there is nothing new to diff a live quote against; the
// oracle relationship is "this closed form was already cross-checked,"
// not "this tick path is cross-checked per tick."
//
// A call and a put at the same strike and expiry share d1 and d2 and both
// cdf evaluations. The first version of this file priced them as two
// independent contracts, each calling norm_cdf (std::erfc under the hood)
// twice, for 4 erfc calls per strike/expiry pair; measured tick-to-quote
// p99 was 20.075us against a 15us target (docs/quote_engine_output_v1.txt).
// This version pairs them: one d1/d2 and two norm_cdf calls per pair, the
// call's price and delta read directly off cdf(d1)/cdf(d2), and the put's
// price and delta derived from the call's by closed-form put-call parity
// (P = C - S + K*e^{-rT}, delta_put = delta_call - 1), an exact identity
// for European options under Black-Scholes, not an approximation. That
// halves the erfc call count per tick; see README "Findings" for the
// latency this bought.
#pragma once
#include <cmath>

#include "black_scholes.hpp"
#include "quote_chain.hpp"

namespace quote {

struct Quote {
    double theo;
    double bid;
    double ask;
    double delta;
};

// Half-spread as a vol-aware fraction of theoretical value; a fixed cent
// spread would be economically wrong across a chain spanning $0.01 to
// $40+ theos. Not calibrated to any real market maker's quoted spread;
// see README "Honest framing".
constexpr double HALF_SPREAD_FRAC = 0.0025;

inline void fill_quote(double theo, double delta, Quote& out) {
    out.theo = theo;
    out.delta = delta;
    const double half = theo * HALF_SPREAD_FRAC;
    out.bid = theo - half;
    out.ask = theo + half;
}

// Reprices the full chain (a call and a put per pair) into a caller-owned,
// preallocated buffer sized 2*chain.size(), ordered [call_0, put_0,
// call_1, put_1, ...]. No allocation happens in this function so it can
// sit in a latency-measured hot path without the allocator contributing
// jitter.
inline void reprice_chain(const std::vector<Pair>& chain, double S, double r, double sigma,
                           Quote* out) {
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const Pair& pr = chain[i];
        const double d1 = (std::log(S / pr.K) + (r + 0.5 * sigma * sigma) * pr.T) / (sigma * pr.sqrtT);
        const double d2 = d1 - sigma * pr.sqrtT;
        const double cdf1 = bs::norm_cdf(d1);
        const double cdf2 = bs::norm_cdf(d2);

        const double call_theo = S * cdf1 - pr.discK * cdf2;
        const double call_delta = cdf1;
        fill_quote(call_theo, call_delta, out[2 * i]);

        // Put-call parity, exact for European options under Black-Scholes:
        // no second pair of erfc calls.
        const double put_theo = call_theo - S + pr.discK;
        const double put_delta = call_delta - 1.0;
        fill_quote(put_theo, put_delta, out[2 * i + 1]);
    }
}

} // namespace quote
