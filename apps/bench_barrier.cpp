// Prices down-and-out and down-and-in calls/puts sharing the same paths and
// checks the in-out parity identity, plus an independent statistical check
// against the closed-form / independently-seeded vanilla MC price.
//
// Usage: bench_barrier <paths> <steps> <seed>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "barrier.hpp"
#include "black_scholes.hpp"
#include "mc_engine.hpp"

int main(int argc, char** argv) {
    long N = (argc > 1) ? std::atol(argv[1]) : 1000000L;
    int steps = (argc > 2) ? std::atoi(argv[2]) : 200;
    unsigned long seed = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 2026UL;

    const double S0 = 100.0, K = 100.0, B = 85.0, r = 0.03, sigma = 0.20, T = 1.0;

    auto run = [&](const char* label, mc::OptionType type) {
        barrier::BarrierResult res =
            barrier::price_down_barrier(type, S0, K, B, r, sigma, T, N, steps, seed);

        // Exact per-path algebraic identity: down-out + down-in == vanilla,
        // computed on the SAME paths, so this must hold to floating-point
        // precision, not just "within Monte Carlo noise".
        double exactDiff =
            std::fabs(res.vanilla_shared_price - (res.down_out_price + res.down_in_price));

        // Independent statistical check: vanilla priced on a different seed
        // (so this comparison genuinely carries MC noise), plus closed-form.
        mc::MCResult vanillaIndep =
            mc::price_reduced(type, S0, K, r, sigma, T, N, seed + 777777UL);
        bs::BSParams p{S0, K, r, sigma, T};
        double bsPrice = (type == mc::OptionType::Call) ? bs::call_price(p) : bs::put_price(p);
        double combinedSE = std::sqrt(res.down_out_stderr * res.down_out_stderr +
                                       res.down_in_stderr * res.down_in_stderr);
        double statDiff = std::fabs(res.vanilla_shared_price - vanillaIndep.price);

        std::printf("%s: down-out=%.6f (se=%.6f)  down-in=%.6f (se=%.6f)\n", label,
                    res.down_out_price, res.down_out_stderr, res.down_in_price,
                    res.down_in_stderr);
        std::printf("%s: shared-path sum=%.6f  exact |diff| vs sum-on-same-paths=%.3e\n", label,
                    res.vanilla_shared_price, exactDiff);
        std::printf(
            "%s: independent-seed vanilla MC=%.6f (se=%.6f)  closed-form BS=%.6f  "
            "|diff|=%.6f  ~%.2f standard errors\n",
            label, vanillaIndep.price, vanillaIndep.stderr_, bsPrice, statDiff,
            statDiff / combinedSE);
    };

    std::printf("paths=%ld, steps=%d, S0=%.1f K=%.1f B=%.1f r=%.2f sigma=%.2f T=%.1f\n", N, steps,
                S0, K, B, r, sigma, T);
    run("down-and-out/in call", mc::OptionType::Call);
    run("down-and-out/in put ", mc::OptionType::Put);
    return 0;
}
