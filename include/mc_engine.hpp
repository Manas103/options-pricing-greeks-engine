// Monte Carlo engine for European options under geometric Brownian motion.
// Provides: naive MC, antithetic + delta control variate MC, pathwise
// Greeks, and common-random-number bump-and-revalue Greeks.
#pragma once
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace mc {

enum class OptionType { Call, Put };

inline double payoff(OptionType type, double ST, double K) {
    if (type == OptionType::Call) return std::max(ST - K, 0.0);
    return std::max(K - ST, 0.0);
}

// Terminal GBM price given a standard normal draw Z.
inline double terminal_price(double S0, double r, double sigma, double T, double Z) {
    return S0 * std::exp((r - 0.5 * sigma * sigma) * T + sigma * std::sqrt(T) * Z);
}

struct MCResult {
    double price;
    double stderr_; // standard error of the price estimator
    long n_paths;   // total simulated paths consumed
};

// Naive MC: N independent paths, one normal draw each.
inline MCResult price_naive(OptionType type, double S0, double K, double r, double sigma,
                             double T, long N, unsigned long seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    double disc = std::exp(-r * T);
    double sum = 0.0, sumsq = 0.0;
    for (long i = 0; i < N; ++i) {
        double Z = nd(rng);
        double ST = terminal_price(S0, r, sigma, T, Z);
        double pay = disc * payoff(type, ST, K);
        sum += pay;
        sumsq += pay * pay;
    }
    double mean = sum / static_cast<double>(N);
    double var = sumsq / static_cast<double>(N) - mean * mean;
    var = std::max(var, 0.0);
    double se = std::sqrt(var / static_cast<double>(N));
    return {mean, se, N};
}

// Antithetic variates + delta (discounted terminal spot) control variate.
// N is the total path budget (must be even); it is spent as N/2 antithetic
// pairs so the total simulated-path count matches price_naive's N exactly,
// making the standard-error comparison apples-to-apples.
inline MCResult price_reduced(OptionType type, double S0, double K, double r, double sigma,
                               double T, long N, unsigned long seed) {
    long M = N / 2;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    double disc = std::exp(-r * T);

    std::vector<double> Y(M), X(M);
    for (long i = 0; i < M; ++i) {
        double Z = nd(rng);
        double ST1 = terminal_price(S0, r, sigma, T, Z);
        double ST2 = terminal_price(S0, r, sigma, T, -Z);
        double pay1 = disc * payoff(type, ST1, K);
        double pay2 = disc * payoff(type, ST2, K);
        double x1 = disc * ST1;
        double x2 = disc * ST2;
        Y[i] = 0.5 * (pay1 + pay2);
        X[i] = 0.5 * (x1 + x2);
    }

    double ymean = 0.0, xmean = 0.0;
    for (long i = 0; i < M; ++i) { ymean += Y[i]; xmean += X[i]; }
    ymean /= static_cast<double>(M);
    xmean /= static_cast<double>(M);

    double covXY = 0.0, varX = 0.0;
    for (long i = 0; i < M; ++i) {
        covXY += (X[i] - xmean) * (Y[i] - ymean);
        varX += (X[i] - xmean) * (X[i] - xmean);
    }
    covXY /= static_cast<double>(M - 1);
    varX /= static_cast<double>(M - 1);
    double beta = (varX > 1e-14) ? covXY / varX : 0.0;

    // E[discounted S_T] under the risk-neutral measure with no dividends is
    // S0 exactly: this is what makes S_T a usable control variate.
    const double EX = S0;

    double sum = 0.0, sumsq = 0.0;
    for (long i = 0; i < M; ++i) {
        double v = Y[i] - beta * (X[i] - EX);
        sum += v;
        sumsq += v * v;
    }
    double mean = sum / static_cast<double>(M);
    double var = sumsq / static_cast<double>(M) - mean * mean;
    var = std::max(var, 0.0);
    double se = std::sqrt(var / static_cast<double>(M));
    return {mean, se, N};
}

struct PathwiseGreeks {
    double delta;
    double vega;
};

// Pathwise (infinitesimal perturbation) estimator: differentiate the
// discounted payoff along each simulated path w.r.t. S0 and sigma.
inline PathwiseGreeks pathwise_greeks(OptionType type, double S0, double K, double r,
                                       double sigma, double T, long N, unsigned long seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    double disc = std::exp(-r * T);
    double sqrtT = std::sqrt(T);
    double sumDelta = 0.0, sumVega = 0.0;
    for (long i = 0; i < N; ++i) {
        double Z = nd(rng);
        double ST = terminal_price(S0, r, sigma, T, Z);
        double dST_dS0 = ST / S0;
        double dST_dsigma = ST * (-sigma * T + sqrtT * Z);
        double ind = 0.0;
        if (type == OptionType::Call) {
            ind = (ST > K) ? 1.0 : 0.0;
        } else {
            ind = (ST < K) ? -1.0 : 0.0;
        }
        sumDelta += disc * ind * dST_dS0;
        sumVega += disc * ind * dST_dsigma;
    }
    return {sumDelta / static_cast<double>(N), sumVega / static_cast<double>(N)};
}

// Bump-and-revalue with common random numbers: the same Z draws are reused
// for the base, up-bumped, and down-bumped simulations so the comparison
// against pathwise Greeks isolates the estimator, not path noise.
inline PathwiseGreeks bump_reval_greeks(OptionType type, double S0, double K, double r,
                                         double sigma, double T, long N, unsigned long seed,
                                         double hS, double hSigma) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<double> Z(N);
    for (long i = 0; i < N; ++i) Z[i] = nd(rng);

    double disc = std::exp(-r * T);
    auto price_for = [&](double S0_, double sigma_) {
        double sum = 0.0;
        for (long i = 0; i < N; ++i) {
            double ST = terminal_price(S0_, r, sigma_, T, Z[i]);
            sum += disc * payoff(type, ST, K);
        }
        return sum / static_cast<double>(N);
    };

    double pUpS = price_for(S0 + hS, sigma);
    double pDnS = price_for(S0 - hS, sigma);
    double delta = (pUpS - pDnS) / (2.0 * hS);

    double pUpV = price_for(S0, sigma + hSigma);
    double pDnV = price_for(S0, sigma - hSigma);
    double vega = (pUpV - pDnV) / (2.0 * hSigma);

    return {delta, vega};
}

} // namespace mc
