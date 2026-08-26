// Generates a simulated listed option chain (there is no real market data
// anywhere in this repository), recovers implied vol per quote through the
// bisection solver in implied_vol.hpp, fits a raw SVI slice per expiry with
// butterfly convexity and calendar monotonicity enforced as hard
// constraints during the fit (svi.hpp), and reports the median absolute
// error between the fitted surface and the quoted vols.
//
// Usage: svi_fit [n_expiries] [n_strikes_per_expiry] [seed] [out_chain_csv]
//                 [out_params_csv]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "black_scholes.hpp"
#include "implied_vol.hpp"
#include "mc_engine.hpp"
#include "svi.hpp"

namespace {

struct Quote {
    int expiry_idx;
    double T;
    double K;
    double F;      // forward, S0*exp(rT), no dividends (q=0), consistent with the rest of the repo
    double k;      // log-moneyness ln(K/F)
    mc::OptionType type;
    double true_vol;   // the surface used to GENERATE this quote, known only because the data is synthetic
    double gen_vol;     // true_vol + microstructure noise: what actually got priced
    double market_vol;  // gen_vol recovered by inverting the priced quote through ivol::solve
    double bid_price, ask_price, mid_price;
    double half_spread_price;
};

// Term structure of the level/skew/curvature of the GENERATING vol surface.
// This is not SVI; it is a separate hand-written function so the SVI fit is
// not fitting its own functional form back to itself.
double base_level(double T) { return 0.22 - 0.02 * T; }
double skew(double T) { return -0.06 / std::sqrt(std::max(T, 0.05)); }
double curvature(double T) { return 0.05 / (T + 0.25); }

double true_vol_at(double k, double T) {
    double v = base_level(T) + skew(T) * k + curvature(T) * k * k;
    return std::max(v, 0.03);
}

} // namespace

int main(int argc, char** argv) {
    int nExpiries = (argc > 1) ? std::atoi(argv[1]) : 20;
    int nStrikes = (argc > 2) ? std::atoi(argv[2]) : 300;
    unsigned long seed = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 2027UL;
    std::string outChain = (argc > 4) ? argv[4] : "docs/synthetic_chain.csv";
    std::string outParams = (argc > 5) ? argv[5] : "docs/svi_params.csv";

    const double S0 = 100.0, r = 0.025;

    // 20 roughly-monthly-to-quarterly expiries out to 2 years; 300 strikes
    // each, evenly spaced in log-moneyness from -0.40 to +0.35 (spot moves
    // of roughly 33% down to 42% up), one quote per strike (put below the
    // forward, call at/above it, the standard OTM-quoting convention for a
    // listed vol surface). 20 x 300 = 6000 quotes total.
    std::vector<double> expiries(static_cast<size_t>(nExpiries));
    for (int i = 0; i < nExpiries; ++i) {
        double frac = (nExpiries == 1) ? 0.0 : static_cast<double>(i) / (nExpiries - 1);
        expiries[static_cast<size_t>(i)] = (1.0 / 12.0) + (2.0 - 1.0 / 12.0) * frac;
    }
    std::vector<double> kgrid(static_cast<size_t>(nStrikes));
    for (int j = 0; j < nStrikes; ++j) {
        double frac = (nStrikes == 1) ? 0.0 : static_cast<double>(j) / (nStrikes - 1);
        kgrid[static_cast<size_t>(j)] = -0.40 + (0.35 - (-0.40)) * frac;
    }

    std::mt19937_64 rng(seed);
    // Vol-point microstructure noise on the generating vol before pricing,
    // and a bid-ask spread that widens away from the money and with tenor,
    // both standard, disclosed features of a synthetic-but-listed-shaped
    // chain (see README, "Honest framing").
    std::normal_distribution<double> noise(0.0, 0.0018); // ~0.18 vol points, 1 std dev

    std::vector<Quote> quotes;
    quotes.reserve(static_cast<size_t>(nExpiries) * static_cast<size_t>(nStrikes));

    double max_roundtrip_err = 0.0;
    int inversion_failures = 0;

    for (int i = 0; i < nExpiries; ++i) {
        double T = expiries[static_cast<size_t>(i)];
        double F = S0 * std::exp(r * T);
        for (int j = 0; j < nStrikes; ++j) {
            double k = kgrid[static_cast<size_t>(j)];
            double K = F * std::exp(k);
            mc::OptionType type = (K >= F) ? mc::OptionType::Call : mc::OptionType::Put;

            double tv = true_vol_at(k, T);
            double gv = std::max(tv + noise(rng), 0.02);

            bs::BSParams p{S0, K, r, gv, T};
            double price = (type == mc::OptionType::Call) ? bs::call_price(p) : bs::put_price(p);

            ivol::ImpliedVolResult iv = ivol::solve(type, price, S0, K, r, T);
            double market_vol = iv.converged ? iv.vol : gv;
            if (!iv.converged) ++inversion_failures;
            else max_roundtrip_err = std::max(max_roundtrip_err, std::fabs(iv.vol - gv));

            double spread_vol = 0.010 + 0.020 * std::fabs(k) + 0.006 * T; // decimal vol, e.g. 0.010 = 1.0 vol pt
            double bid_vol = std::max(market_vol - 0.5 * spread_vol, 0.01);
            double ask_vol = market_vol + 0.5 * spread_vol;
            bs::BSParams pb{S0, K, r, bid_vol, T};
            bs::BSParams pa{S0, K, r, ask_vol, T};
            double bid_price = (type == mc::OptionType::Call) ? bs::call_price(pb) : bs::put_price(pb);
            double ask_price = (type == mc::OptionType::Call) ? bs::call_price(pa) : bs::put_price(pa);

            Quote q{};
            q.expiry_idx = i;
            q.T = T;
            q.K = K;
            q.F = F;
            q.k = k;
            q.type = type;
            q.true_vol = tv;
            q.gen_vol = gv;
            q.market_vol = market_vol;
            q.bid_price = bid_price;
            q.ask_price = ask_price;
            q.mid_price = 0.5 * (bid_price + ask_price);
            q.half_spread_price = 0.5 * (ask_price - bid_price);
            quotes.push_back(q);
        }
    }

    std::printf("generated %zu quotes: %d expiries x %d strikes, seed=%lu\n", quotes.size(),
                nExpiries, nStrikes, seed);
    std::printf("implied-vol round trip (gen_vol -> BS price -> ivol::solve): max abs error = %.3e, "
                "inversion failures = %d\n", max_roundtrip_err, inversion_failures);

    // Fit one SVI slice per expiry, in increasing T order, each one's
    // calendar floor built from the previous slice's fitted total variance
    // on the shared kgrid.
    std::vector<svi::Params> fitted(static_cast<size_t>(nExpiries));
    std::vector<bool> feasible(static_cast<size_t>(nExpiries), false);
    std::vector<double> prev_floor_w;
    bool any_infeasible = false;

    for (int i = 0; i < nExpiries; ++i) {
        double T = expiries[static_cast<size_t>(i)];
        std::vector<svi::FitPoint> pts;
        pts.reserve(static_cast<size_t>(nStrikes));
        for (const auto& q : quotes) {
            if (q.expiry_idx != i) continue;
            pts.push_back({q.k, q.market_vol * q.market_vol * T, 1.0});
        }

        svi::Params init;
        if (i == 0) {
            double meanw = 0.0;
            for (auto& pt : pts) meanw += pt.w;
            meanw /= static_cast<double>(pts.size());
            init = svi::Params{meanw * 0.9, 0.15, -0.35, 0.0, 0.15};
        } else {
            init = fitted[static_cast<size_t>(i - 1)];
            init.a += 0.001; // nudge level up so the warm start clears the previous slice's floor
        }

        const std::vector<double>* floorGridPtr = (i == 0) ? nullptr : &kgrid;
        const std::vector<double>* floorWPtr = (i == 0) ? nullptr : &prev_floor_w;

        svi::FitResult fr = svi::fit_slice(pts, kgrid, init, floorGridPtr, floorWPtr);
        fitted[static_cast<size_t>(i)] = fr.params;
        feasible[static_cast<size_t>(i)] = fr.feasible;
        if (!fr.feasible) any_infeasible = true;

        prev_floor_w.resize(kgrid.size());
        for (size_t g = 0; g < kgrid.size(); ++g) {
            prev_floor_w[g] = svi::total_variance(fr.params, kgrid[g]);
        }

        std::printf("expiry %2d T=%.4f: a=%.6f b=%.6f rho=%.4f m=%.4f sigma=%.4f  sse=%.6e  "
                    "feasible=%s  iters=%d\n",
                    i, T, fr.params.a, fr.params.b, fr.params.rho, fr.params.m, fr.params.sigma,
                    fr.sse, fr.feasible ? "true" : "false", fr.iterations);
    }

    // Independent verification pass, on a grid 4x finer than the fit grid,
    // checking both hard constraints across the WHOLE fitted surface. This
    // is the brute-force check the fit's own elitist-feasible mechanism
    // already guarantees; running it again here, at higher resolution and
    // outside the optimizer, is what makes "zero violations" a measured
    // fact rather than an assumption. python/validate_svi.py repeats this a
    // third time, independently, in a different language.
    std::vector<double> fine_grid(static_cast<size_t>(nStrikes) * 4);
    for (size_t j = 0; j < fine_grid.size(); ++j) {
        double frac = static_cast<double>(j) / static_cast<double>(fine_grid.size() - 1);
        fine_grid[j] = -0.40 + (0.35 - (-0.40)) * frac;
    }
    int butterfly_violations = 0;
    int calendar_violations = 0;
    for (int i = 0; i < nExpiries; ++i) {
        if (!svi::is_convex(fitted[static_cast<size_t>(i)], fine_grid, 1e-7)) ++butterfly_violations;
    }
    for (int i = 1; i < nExpiries; ++i) {
        std::vector<double> floorw(fine_grid.size());
        for (size_t g = 0; g < fine_grid.size(); ++g) {
            floorw[g] = svi::total_variance(fitted[static_cast<size_t>(i - 1)], fine_grid[g]);
        }
        if (!svi::respects_calendar_floor(fitted[static_cast<size_t>(i)], fine_grid, floorw, 1e-7)) {
            ++calendar_violations;
        }
    }

    // Median absolute error between the fitted surface and the quoted
    // (market) vol, across all 6000 quotes, in vol points (vol * 100).
    std::vector<double> abs_err_pts;
    abs_err_pts.reserve(quotes.size());
    for (auto& q : quotes) {
        double w_fit = svi::total_variance(fitted[static_cast<size_t>(q.expiry_idx)], q.k);
        double fit_vol = std::sqrt(std::max(w_fit, 0.0) / q.T);
        abs_err_pts.push_back(std::fabs(fit_vol - q.market_vol) * 100.0);
    }
    std::vector<double> sorted_err = abs_err_pts;
    std::sort(sorted_err.begin(), sorted_err.end());
    double median_err = sorted_err[sorted_err.size() / 2];
    if (sorted_err.size() % 2 == 0) {
        median_err = 0.5 * (sorted_err[sorted_err.size() / 2 - 1] + sorted_err[sorted_err.size() / 2]);
    }

    std::printf("\nbutterfly (convexity) violations on %zu-point verification grid, %d expiries: %d\n",
                fine_grid.size(), nExpiries, butterfly_violations);
    std::printf("calendar (monotonicity) violations across %d adjacent expiry pairs: %d\n",
                nExpiries - 1, calendar_violations);
    std::printf("any expiry returned infeasible by fit_slice itself: %s\n",
                any_infeasible ? "true" : "false");
    std::printf("median absolute error, fitted SVI vs quoted vol, %zu quotes: %.4f vol points\n",
                quotes.size(), median_err);

    std::ofstream chainCsv(outChain);
    chainCsv << "expiry_idx,T,K,F,k,type,true_vol,gen_vol,market_vol,fit_vol,abs_err_vol_pts,"
                "bid_price,ask_price,mid_price,half_spread_price\n";
    for (size_t idx = 0; idx < quotes.size(); ++idx) {
        const auto& q = quotes[idx];
        double w_fit = svi::total_variance(fitted[static_cast<size_t>(q.expiry_idx)], q.k);
        double fit_vol = std::sqrt(std::max(w_fit, 0.0) / q.T);
        chainCsv << q.expiry_idx << "," << q.T << "," << q.K << "," << q.F << "," << q.k << ","
                 << (q.type == mc::OptionType::Call ? "C" : "P") << "," << q.true_vol << ","
                 << q.gen_vol << "," << q.market_vol << "," << fit_vol << "," << abs_err_pts[idx]
                 << "," << q.bid_price << "," << q.ask_price << "," << q.mid_price << ","
                 << q.half_spread_price << "\n";
    }
    chainCsv.close();

    std::ofstream paramsCsv(outParams);
    paramsCsv << "expiry_idx,T,a,b,rho,m,sigma,feasible\n";
    for (int i = 0; i < nExpiries; ++i) {
        const auto& p = fitted[static_cast<size_t>(i)];
        paramsCsv << i << "," << expiries[static_cast<size_t>(i)] << "," << p.a << "," << p.b << ","
                   << p.rho << "," << p.m << "," << p.sigma << ","
                   << (feasible[static_cast<size_t>(i)] ? 1 : 0) << "\n";
    }
    paramsCsv.close();

    std::printf("\nchain written to: %s\n", outChain.c_str());
    std::printf("SVI params written to: %s\n", outParams.c_str());
    return 0;
}
