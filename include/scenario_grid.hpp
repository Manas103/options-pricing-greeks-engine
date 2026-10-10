// The scenario set: historical (bootstrapped from a simulated, correlated
// daily spot-return / vol-change history, the leverage-effect correlation
// stated explicitly below) plus hypothetical (a dense, deliberately wider
// grid reaching combinations the historical draw never produced). Every
// parameter that controls either set is a field on ScenarioManifest so a run
// is reproducible from the manifest file alone (see apps/scenario_var.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace scenario {

struct Scenario {
    double spot_shock; // fraction, e.g. -0.10 = down 10%
    double vol_shift;  // vol points, additive
    bool is_historical;
};

struct ScenarioManifest {
    uint64_t book_seed = 7;
    uint64_t historical_seed = 2027;
    int n_historical = 2000;
    int n_hyp_spot = 500;
    int n_hyp_vol = 476; // 500 * 476 = 238,000; + 2,000 historical = 240,000
    // Asymmetric and downside-weighted, matching how equity stress scenarios
    // are conventionally built (a -2008-style crash is a far bigger, more
    // examined tail than a comparable rally): down moves run almost 3x
    // further than up moves, and vol shocks are allowed much further up
    // than down, since vol spikes, it rarely collapses in a single day.
    double hyp_spot_lo = -0.25;
    double hyp_spot_hi = 0.10;
    double hyp_vol_lo = -0.06;
    double hyp_vol_hi = 0.35;
    double hist_spot_sigma = 0.012;  // 1.2% daily spot-return vol
    double hist_vol_sigma = 0.010;   // 1.0 vol-point daily vol-of-vol
    double hist_corr = -0.6;         // leverage effect: spot down <-> vol up
};

inline std::vector<Scenario> generate_historical(const ScenarioManifest& m) {
    std::mt19937_64 rng(m.historical_seed);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<Scenario> out;
    out.reserve(m.n_historical);
    double rho = m.hist_corr;
    double orth = std::sqrt(std::max(0.0, 1.0 - rho * rho));
    for (int i = 0; i < m.n_historical; ++i) {
        double z1 = z(rng);
        double z2 = z(rng);
        double spot_ret = m.hist_spot_sigma * z1;
        double vol_chg = m.hist_vol_sigma * (rho * z1 + orth * z2);
        out.push_back({spot_ret, vol_chg, true});
    }
    return out;
}

inline std::vector<Scenario> generate_hypothetical(const ScenarioManifest& m) {
    std::vector<Scenario> out;
    out.reserve(static_cast<size_t>(m.n_hyp_spot) * m.n_hyp_vol);
    for (int i = 0; i < m.n_hyp_spot; ++i) {
        double spot_shock =
            m.hyp_spot_lo + (m.hyp_spot_hi - m.hyp_spot_lo) * i / (m.n_hyp_spot - 1);
        for (int j = 0; j < m.n_hyp_vol; ++j) {
            double vol_shift =
                m.hyp_vol_lo + (m.hyp_vol_hi - m.hyp_vol_lo) * j / (m.n_hyp_vol - 1);
            out.push_back({spot_shock, vol_shift, false});
        }
    }
    return out;
}

inline std::vector<Scenario> generate_all(const ScenarioManifest& m) {
    std::vector<Scenario> hist = generate_historical(m);
    std::vector<Scenario> hyp = generate_hypothetical(m);
    std::vector<Scenario> all;
    all.reserve(hist.size() + hyp.size());
    all.insert(all.end(), hist.begin(), hist.end());
    all.insert(all.end(), hyp.begin(), hyp.end());
    return all;
}

} // namespace scenario
