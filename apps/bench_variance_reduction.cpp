// Measures the standard-error reduction from antithetic variates + delta
// control variate versus naive MC, at equal path count, on an ATM European
// call. Runs many independent replications and reports two agreeing
// measurements: (a) the average of each run's own analytic standard error,
// and (b) the empirical standard deviation of the price estimate ACROSS
// replications, which is a direct, assumption-free measurement of standard
// error.
//
// Usage: bench_variance_reduction <n_replications> <paths_per_replication> <seed> [K] [sigma] [T]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mc_engine.hpp"

int main(int argc, char** argv) {
    int R = (argc > 1) ? std::atoi(argv[1]) : 50;
    long N = (argc > 2) ? std::atol(argv[2]) : 200000L;
    unsigned long baseSeed = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 7UL;

    const double S0 = 100.0;
    const double K = (argc > 4) ? std::atof(argv[4]) : 100.0;
    const double r = 0.03;
    const double sigma = (argc > 5) ? std::atof(argv[5]) : 0.20;
    const double T = (argc > 6) ? std::atof(argv[6]) : 1.0;

    std::vector<double> naivePrices(R), reducedPrices(R);
    double naiveSeAvg = 0.0, reducedSeAvg = 0.0;

    for (int i = 0; i < R; ++i) {
        unsigned long seed = baseSeed + 998244353UL * static_cast<unsigned long>(i);
        mc::MCResult naive = mc::price_naive(mc::OptionType::Call, S0, K, r, sigma, T, N, seed);
        mc::MCResult reduced =
            mc::price_reduced(mc::OptionType::Call, S0, K, r, sigma, T, N, seed);
        naivePrices[i] = naive.price;
        reducedPrices[i] = reduced.price;
        naiveSeAvg += naive.stderr_;
        reducedSeAvg += reduced.stderr_;
    }
    naiveSeAvg /= R;
    reducedSeAvg /= R;

    auto empirical_std = [&](const std::vector<double>& v) {
        double mean = 0.0;
        for (double x : v) mean += x;
        mean /= v.size();
        double var = 0.0;
        for (double x : v) var += (x - mean) * (x - mean);
        var /= (v.size() - 1);
        return std::sqrt(var);
    };

    double naiveEmpStd = empirical_std(naivePrices);
    double reducedEmpStd = empirical_std(reducedPrices);

    std::printf("replications=%d, paths_per_replication=%ld\n", R, N);
    std::printf("naive:   analytic SE (avg over runs) = %.6f, empirical std across runs = %.6f\n",
                naiveSeAvg, naiveEmpStd);
    std::printf("reduced: analytic SE (avg over runs) = %.6f, empirical std across runs = %.6f\n",
                reducedSeAvg, reducedEmpStd);
    std::printf("SE reduction ratio (analytic avg)   : %.3fx\n", naiveSeAvg / reducedSeAvg);
    std::printf("SE reduction ratio (empirical)       : %.3fx\n", naiveEmpStd / reducedEmpStd);
    return 0;
}
