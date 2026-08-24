// Down-and-out / down-and-in barrier payoffs on the same GBM path engine,
// discretely monitored. Down-and-out and down-and-in are simulated on the
// SAME path draws, so for every path exactly one of them pays the vanilla
// payoff and the other pays zero: summed across paths they reproduce the
// vanilla European MC price as an exact per-path algebraic identity (not
// merely "within Monte Carlo noise"). See README for why that is a
// stronger check than an independent-path parity comparison, and for the
// separate, genuinely statistical comparison against the closed-form /
// independently-seeded vanilla MC price.
#pragma once
#include <cmath>
#include <random>
#include "mc_engine.hpp"

namespace barrier {

using mc::OptionType;

struct BarrierResult {
    double down_out_price;
    double down_out_stderr;
    double down_in_price;
    double down_in_stderr;
    double vanilla_shared_price;   // mean of (down_out + down_in) per path, on shared paths
    long n_paths;
};

// B is the barrier level; the option knocks when S touches or crosses B
// from above (down barrier). Monitored at `steps` discretization points
// across [0, T]. Uses one RNG stream so down-in/down-out/vanilla-shared are
// evaluated on identical simulated paths.
inline BarrierResult price_down_barrier(OptionType type, double S0, double K, double B,
                                         double r, double sigma, double T, long N, int steps,
                                         unsigned long seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    double dt = T / static_cast<double>(steps);
    double disc = std::exp(-r * T);
    double drift = (r - 0.5 * sigma * sigma) * dt;
    double vol = sigma * std::sqrt(dt);

    double sumOut = 0.0, sumsqOut = 0.0;
    double sumIn = 0.0, sumsqIn = 0.0;
    double sumShared = 0.0;

    for (long p = 0; p < N; ++p) {
        double S = S0;
        bool hit = (S <= B);
        // Keep evolving S all the way to T regardless of `hit`. An earlier
        // version stopped the path the instant it touched the barrier,
        // which froze S at ~B for the payoff instead of letting it continue
        // to the real terminal price -- see README, "The bug worth reading
        // about", for how that surfaced (a down-and-in call priced at
        // exactly 0.000000 with zero variance, which a ~40% touch
        // probability makes essentially impossible).
        for (int t = 0; t < steps; ++t) {
            double Z = nd(rng);
            S = S * std::exp(drift + vol * Z);
            if (S <= B) hit = true;
        }
        double vanillaPay = disc * mc::payoff(type, S, K);
        double outPay = hit ? 0.0 : vanillaPay;
        double inPay = hit ? vanillaPay : 0.0;

        sumOut += outPay;
        sumsqOut += outPay * outPay;
        sumIn += inPay;
        sumsqIn += inPay * inPay;
        sumShared += outPay + inPay; // == vanillaPay exactly, kept as a running check
    }

    double n = static_cast<double>(N);
    double meanOut = sumOut / n;
    double varOut = std::max(sumsqOut / n - meanOut * meanOut, 0.0);
    double meanIn = sumIn / n;
    double varIn = std::max(sumsqIn / n - meanIn * meanIn, 0.0);

    BarrierResult res{};
    res.down_out_price = meanOut;
    res.down_out_stderr = std::sqrt(varOut / n);
    res.down_in_price = meanIn;
    res.down_in_stderr = std::sqrt(varIn / n);
    res.vanilla_shared_price = sumShared / n;
    res.n_paths = N;
    return res;
}

} // namespace barrier
