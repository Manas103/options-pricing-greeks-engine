// Closed-form Black-Scholes pricer and Greeks, implemented independently of
// the Monte Carlo engine so it can serve as ground truth to diff against.
// No dividend yield term (q = 0): kept out deliberately to keep the model
// surface small and auditable; see README for the tradeoff.
#pragma once
#include <cmath>

namespace bs {

inline double norm_cdf(double x) {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
}

inline double norm_pdf(double x) {
    static const double inv_sqrt_2pi = 0.3989422804014327;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

struct BSParams {
    double S;     // spot
    double K;     // strike
    double r;     // risk-free rate (continuously compounded)
    double sigma; // volatility
    double T;     // time to maturity, years
};

inline double d1(const BSParams& p) {
    return (std::log(p.S / p.K) + (p.r + 0.5 * p.sigma * p.sigma) * p.T) /
           (p.sigma * std::sqrt(p.T));
}

inline double d2(const BSParams& p) {
    return d1(p) - p.sigma * std::sqrt(p.T);
}

inline double call_price(const BSParams& p) {
    double D1 = d1(p), D2 = d2(p);
    return p.S * norm_cdf(D1) - p.K * std::exp(-p.r * p.T) * norm_cdf(D2);
}

inline double put_price(const BSParams& p) {
    double D1 = d1(p), D2 = d2(p);
    return p.K * std::exp(-p.r * p.T) * norm_cdf(-D2) - p.S * norm_cdf(-D1);
}

struct Greeks {
    double delta;
    double gamma;
    double vega;
    double theta;
};

inline Greeks call_greeks(const BSParams& p) {
    double D1 = d1(p), D2 = d2(p);
    double sqrtT = std::sqrt(p.T);
    Greeks g{};
    g.delta = norm_cdf(D1);
    g.gamma = norm_pdf(D1) / (p.S * p.sigma * sqrtT);
    g.vega  = p.S * norm_pdf(D1) * sqrtT;
    g.theta = -(p.S * norm_pdf(D1) * p.sigma) / (2.0 * sqrtT) -
              p.r * p.K * std::exp(-p.r * p.T) * norm_cdf(D2);
    return g;
}

inline Greeks put_greeks(const BSParams& p) {
    double D1 = d1(p), D2 = d2(p);
    double sqrtT = std::sqrt(p.T);
    Greeks g{};
    g.delta = norm_cdf(D1) - 1.0;
    g.gamma = norm_pdf(D1) / (p.S * p.sigma * sqrtT);
    g.vega  = p.S * norm_pdf(D1) * sqrtT;
    g.theta = -(p.S * norm_pdf(D1) * p.sigma) / (2.0 * sqrtT) +
              p.r * p.K * std::exp(-p.r * p.T) * norm_cdf(-D2);
    return g;
}

} // namespace bs
