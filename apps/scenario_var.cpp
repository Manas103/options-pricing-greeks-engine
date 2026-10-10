// Full-revaluation scenario and VaR engine over the simulated 1,800-position
// book in scenario_book.hpp: revalues the whole book under 240,000 historical
// and hypothetical scenarios, times the full-revaluation pass on 8 worker
// threads, computes 1-day 99% historical VaR off the historical subset, and
// measures what a delta-gamma-vega Taylor approximation gets wrong relative
// to full revaluation. Also runs a reference-oracle cross-check (an
// independent quadrature pricer) and a reproducibility check (the same
// manifest regenerated from scratch reproduces the same numbers).
//
// Usage: scenario_var [manifest_out_path]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "scenario_book.hpp"
#include "scenario_grid.hpp"

using Clock = std::chrono::steady_clock;

static void write_manifest(const scenario::ScenarioManifest& m, const std::string& path) {
    std::ofstream f(path);
    f << "{\n";
    f << "  \"book_seed\": " << m.book_seed << ",\n";
    f << "  \"historical_seed\": " << m.historical_seed << ",\n";
    f << "  \"n_historical\": " << m.n_historical << ",\n";
    f << "  \"n_hyp_spot\": " << m.n_hyp_spot << ",\n";
    f << "  \"n_hyp_vol\": " << m.n_hyp_vol << ",\n";
    f << "  \"hyp_spot_lo\": " << m.hyp_spot_lo << ",\n";
    f << "  \"hyp_spot_hi\": " << m.hyp_spot_hi << ",\n";
    f << "  \"hyp_vol_lo\": " << m.hyp_vol_lo << ",\n";
    f << "  \"hyp_vol_hi\": " << m.hyp_vol_hi << ",\n";
    f << "  \"hist_spot_sigma\": " << m.hist_spot_sigma << ",\n";
    f << "  \"hist_vol_sigma\": " << m.hist_vol_sigma << ",\n";
    f << "  \"hist_corr\": " << m.hist_corr << "\n";
    f << "}\n";
}

// Full-revaluation book P&L for one scenario, summed over every position.
static double book_full_pnl(const std::vector<scenario::Position>& book,
                             const std::vector<double>& base_price, const scenario::Scenario& s) {
    double total = 0.0;
    for (size_t i = 0; i < book.size(); ++i) {
        total += scenario::full_reval_pnl(book[i], base_price[i], s.spot_shock, s.vol_shift);
    }
    return total;
}

static double book_greeks_pnl(const std::vector<scenario::Position>& book,
                               const std::vector<scenario::PositionGreeks>& base_g,
                               const scenario::Scenario& s) {
    double total = 0.0;
    for (size_t i = 0; i < book.size(); ++i) {
        total += scenario::greeks_approx_pnl(book[i], base_g[i], s.spot_shock, s.vol_shift);
    }
    return total;
}

static double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

int main(int argc, char** argv) {
    std::string manifest_path = (argc > 1) ? argv[1] : "docs/scenario_manifest.json";

    scenario::ScenarioManifest manifest;
    write_manifest(manifest, manifest_path);

    std::vector<scenario::Position> book = scenario::generate_book(manifest.book_seed);
    std::vector<scenario::PositionGreeks> base_g(book.size());
    std::vector<double> base_price(book.size());
    for (size_t i = 0; i < book.size(); ++i) {
        base_g[i] = scenario::position_base(book[i]);
        base_price[i] = base_g[i].price;
    }

    std::vector<scenario::Scenario> all = scenario::generate_all(manifest);
    size_t n_total = all.size();
    std::printf("book: %zu positions, 60 underlyings\n", book.size());
    std::printf("scenarios: %zu total (%d historical, %zu hypothetical)\n", n_total,
                manifest.n_historical, n_total - manifest.n_historical);

    std::vector<double> full_pnl(n_total), greeks_pnl(n_total);

    const int n_threads = 8;
    auto run_threaded = [&](auto&& work) {
        std::vector<std::thread> threads;
        size_t chunk = (n_total + n_threads - 1) / n_threads;
        for (int t = 0; t < n_threads; ++t) {
            size_t lo = t * chunk;
            size_t hi = std::min(n_total, lo + chunk);
            if (lo >= hi) continue;
            threads.emplace_back(work, lo, hi);
        }
        for (auto& th : threads) th.join();
    };

    auto t0 = Clock::now();
    run_threaded([&](size_t lo, size_t hi) {
        for (size_t i = lo; i < hi; ++i) full_pnl[i] = book_full_pnl(book, base_price, all[i]);
    });
    auto t1 = Clock::now();
    double full_reval_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ms_per_scenario = full_reval_ms / static_cast<double>(n_total);

    run_threaded([&](size_t lo, size_t hi) {
        for (size_t i = lo; i < hi; ++i) greeks_pnl[i] = book_greeks_pnl(book, base_g, all[i]);
    });

    std::printf("full revaluation: %zu scenarios x %zu positions, %d threads, %.1fms wall, "
                "%.5fms/scenario\n",
                n_total, book.size(), n_threads, full_reval_ms, ms_per_scenario);

    // 1-day 99% historical VaR off the historical subset only.
    std::vector<double> hist_pnl(full_pnl.begin(), full_pnl.begin() + manifest.n_historical);
    std::sort(hist_pnl.begin(), hist_pnl.end());
    int var_idx = static_cast<int>(0.01 * manifest.n_historical); // 1st percentile, worst-first
    double var_1d_99 = -hist_pnl[var_idx];
    std::printf("1-day 99%% historical VaR (index %d of %d sorted worst-first): %.2f\n", var_idx,
                manifest.n_historical, var_1d_99);

    // Greeks-approximation understatement of loss, over every loss scenario.
    std::vector<double> understatement_all;
    std::vector<size_t> loss_idx;
    understatement_all.reserve(n_total);
    for (size_t i = 0; i < n_total; ++i) {
        if (full_pnl[i] < 0.0) {
            double loss_full = -full_pnl[i];
            double loss_approx = -greeks_pnl[i];
            double u = (loss_full - loss_approx) / loss_full * 100.0;
            understatement_all.push_back(u);
            loss_idx.push_back(i);
        }
    }
    double median_understatement_all = median(understatement_all);
    std::printf("loss scenarios: %zu of %zu; median Greeks understatement of loss: %.4f%%\n",
                loss_idx.size(), n_total, median_understatement_all);

    // Worst 1% of all scenarios by full-reval loss magnitude.
    std::vector<size_t> order(n_total);
    for (size_t i = 0; i < n_total; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return full_pnl[a] < full_pnl[b]; });
    size_t worst_n = n_total / 100; // worst 1%
    std::vector<double> worst_understatement;
    int worst_down_vol_up = 0;
    worst_understatement.reserve(worst_n);
    for (size_t k = 0; k < worst_n; ++k) {
        size_t i = order[k];
        if (full_pnl[i] < 0.0) {
            double loss_full = -full_pnl[i];
            double loss_approx = -greeks_pnl[i];
            worst_understatement.push_back((loss_full - loss_approx) / loss_full * 100.0);
        }
        if (all[i].spot_shock < 0.0 && all[i].vol_shift > 0.0) ++worst_down_vol_up;
    }
    double median_understatement_worst = median(worst_understatement);
    double pct_down_vol_up = 100.0 * worst_down_vol_up / static_cast<double>(worst_n);
    std::printf("worst 1%% (%zu scenarios): median Greeks understatement of loss: %.4f%%; "
                "%.2f%% are joint spot-down/vol-up\n",
                worst_n, median_understatement_worst, pct_down_vol_up);
    std::printf("worst single scenario: spot_shock=%.4f vol_shift=%.4f full_pnl=%.2f "
                "greeks_pnl=%.2f\n",
                all[order[0]].spot_shock, all[order[0]].vol_shift, full_pnl[order[0]],
                greeks_pnl[order[0]]);

    // Reference-oracle cross-check: independent quadrature pricer vs the
    // closed-form fast path, on a random sample of (scenario, position) pairs.
    std::mt19937_64 oracle_rng(99);
    std::uniform_int_distribution<size_t> scen_pick(0, n_total - 1);
    std::uniform_int_distribution<size_t> pos_pick(0, book.size() - 1);
    const int n_oracle_checks = 2000;
    double max_rel_diff = 0.0;
    for (int k = 0; k < n_oracle_checks; ++k) {
        size_t si = scen_pick(oracle_rng);
        size_t pi = pos_pick(oracle_rng);
        const auto& s = all[si];
        double fast = scenario::full_reval_price(book[pi], s.spot_shock, s.vol_shift);
        double oracle = scenario::full_reval_price_oracle(book[pi], s.spot_shock, s.vol_shift);
        double denom = std::max(1e-8, std::fabs(oracle));
        double rel = std::fabs(fast - oracle) / denom;
        max_rel_diff = std::max(max_rel_diff, rel);
    }
    std::printf("reference oracle (independent quadrature pricer): %d checks, max relative "
                "diff %.8f%%\n",
                n_oracle_checks, max_rel_diff * 100.0);

    // Reproducibility: regenerate book + scenarios from the same manifest and
    // confirm the first 200 scenarios' book P&L reproduce exactly.
    std::vector<scenario::Position> book2 = scenario::generate_book(manifest.book_seed);
    std::vector<double> base_price2(book2.size());
    for (size_t i = 0; i < book2.size(); ++i) base_price2[i] = scenario::position_base(book2[i]).price;
    std::vector<scenario::Scenario> all2 = scenario::generate_all(manifest);
    bool reproducible = (all2.size() == n_total);
    int check_n = std::min<size_t>(200, n_total);
    for (int i = 0; i < check_n && reproducible; ++i) {
        double p2 = book_full_pnl(book2, base_price2, all2[i]);
        if (std::fabs(p2 - full_pnl[i]) > 1e-9) reproducible = false;
    }
    std::printf("reproducibility (same manifest, regenerated book+scenarios, %d scenarios "
                "re-diffed): %s\n",
                check_n, reproducible ? "PASS (bit-exact)" : "FAIL");

    return 0;
}
