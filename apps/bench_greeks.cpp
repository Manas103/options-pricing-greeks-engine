// Computes pathwise Greeks and common-random-number bump-and-revalue Greeks
// side by side for European calls and puts, and reports the max absolute
// difference across delta and vega.
//
// Usage: bench_greeks <paths> <seed>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "mc_engine.hpp"

int main(int argc, char** argv) {
    long N = (argc > 1) ? std::atol(argv[1]) : 4000000L;
    unsigned long seed = (argc > 2) ? std::strtoul(argv[2], nullptr, 10) : 123UL;

    const double S0 = 100.0, K = 100.0, r = 0.03, sigma = 0.20, T = 1.0;
    const double hS = 0.5;      // bump in spot
    const double hSigma = 0.005; // bump in vol

    double maxAbsDiff = 0.0;

    auto report = [&](const char* label, mc::OptionType type) {
        mc::PathwiseGreeks pw = mc::pathwise_greeks(type, S0, K, r, sigma, T, N, seed);
        mc::PathwiseGreeks br =
            mc::bump_reval_greeks(type, S0, K, r, sigma, T, N, seed, hS, hSigma);
        double dDelta = std::fabs(pw.delta - br.delta);
        double dVega = std::fabs(pw.vega - br.vega);
        maxAbsDiff = std::max({maxAbsDiff, dDelta, dVega});
        std::printf("%s: pathwise delta=%.6f  bump delta=%.6f  |diff|=%.6f\n", label, pw.delta,
                    br.delta, dDelta);
        std::printf("%s: pathwise vega =%.6f  bump vega =%.6f  |diff|=%.6f\n", label, pw.vega,
                    br.vega, dVega);
    };

    std::printf("paths=%ld, hS=%.4f, hSigma=%.4f\n", N, hS, hSigma);
    report("call", mc::OptionType::Call);
    report("put ", mc::OptionType::Put);
    std::printf("max abs diff (pathwise vs bump-and-revalue): %.6f\n", maxAbsDiff);
    return 0;
}
