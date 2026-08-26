// Relative-value screener: reads the chain CSV written by svi_fit (fitted
// SVI vol already attached to every quote), ranks residual richness per
// strike, and keeps only the dislocations whose theoretical edge survives a
// half bid-ask spread crossing cost.
//
// Two-stage design, deliberately not one threshold:
//   1. A materiality screen in VOL POINTS: a quote is a "dislocation
//      candidate" only if the fitted surface disagrees with its quoted vol
//      by more than a robust outlier threshold (median absolute residual +
//      2x the median absolute deviation of that residual across the whole
//      surface). This is cheap and catches genuine curve-vs-quote
//      departures, not the ordinary noise the fit is expected to leave
//      behind (see README, Measured Results, for that noise floor).
//   2. An economic screen in PRICE TERMS: only a candidate is a real trade
//      is decided by repricing it at the fitted vol and comparing the
//      resulting theoretical edge against the ACTUAL bid-ask half-spread
//      for that specific quote (not an average spread), because a vol-point
//      disagreement is worth very different amounts of money depending on
//      vega, and the cost of acting on it is that quote's own spread, not
//      the book's average spread.
// Why a half-spread charge and not the full spread: a resting order inside
// the spread, or crossing only one side, is the realistic cost of acting on
// a relative-value signal; charging the full round-trip spread would
// conflate "is this mispriced" with "is this profitable after also closing
// the position at the same unfavorable spread", a different, stricter
// question this screener does not claim to answer.
//
// Usage: screener [chain_csv] [k_mad]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Row {
    int expiry_idx;
    double T, K, F, k;
    char type;
    double true_vol, gen_vol, market_vol, fit_vol, abs_err_vol_pts;
    double bid_price, ask_price, mid_price, half_spread_price;
    double edge_price; // (fit_vol reprice) - mid_price, signed
};

double bs_price_at(double S, double K, double r, double sigma, double T, char type) {
    double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
    double d2 = d1 - sigma * std::sqrt(T);
    auto ncdf = [](double x) { return 0.5 * std::erfc(-x * M_SQRT1_2); };
    if (type == 'C') return S * ncdf(d1) - K * std::exp(-r * T) * ncdf(d2);
    return K * std::exp(-r * T) * ncdf(-d2) - S * ncdf(-d1);
}

} // namespace

int main(int argc, char** argv) {
    std::string chainPath = (argc > 1) ? argv[1] : "docs/synthetic_chain.csv";
    double k_mad = (argc > 2) ? std::atof(argv[2]) : 2.0;
    const double S0 = 100.0, r = 0.025; // must match apps/svi_fit.cpp's generator

    std::ifstream f(chainPath);
    if (!f) {
        std::fprintf(stderr, "could not open %s\n", chainPath.c_str());
        return 1;
    }
    std::string header;
    std::getline(f, header);

    std::vector<Row> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;
        Row row{};
        std::getline(ss, cell, ','); row.expiry_idx = std::atoi(cell.c_str());
        std::getline(ss, cell, ','); row.T = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.K = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.F = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.k = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.type = cell.empty() ? 'C' : cell[0];
        std::getline(ss, cell, ','); row.true_vol = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.gen_vol = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.market_vol = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.fit_vol = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.abs_err_vol_pts = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.bid_price = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.ask_price = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.mid_price = std::atof(cell.c_str());
        std::getline(ss, cell, ','); row.half_spread_price = std::atof(cell.c_str());
        row.edge_price = bs_price_at(S0, row.K, r, row.fit_vol, row.T, row.type) - row.mid_price;
        rows.push_back(row);
    }

    if (rows.empty()) {
        std::fprintf(stderr, "no rows loaded from %s\n", chainPath.c_str());
        return 1;
    }

    // Stage 1: robust materiality threshold on abs_err_vol_pts.
    std::vector<double> errs;
    errs.reserve(rows.size());
    for (auto& row : rows) errs.push_back(row.abs_err_vol_pts);
    std::vector<double> sorted_errs = errs;
    std::sort(sorted_errs.begin(), sorted_errs.end());
    double median_err = sorted_errs[sorted_errs.size() / 2];
    if (sorted_errs.size() % 2 == 0) {
        median_err = 0.5 * (sorted_errs[sorted_errs.size() / 2 - 1] + sorted_errs[sorted_errs.size() / 2]);
    }
    std::vector<double> devs;
    devs.reserve(errs.size());
    for (double e : errs) devs.push_back(std::fabs(e - median_err));
    std::sort(devs.begin(), devs.end());
    double mad = devs[devs.size() / 2];
    if (devs.size() % 2 == 0) mad = 0.5 * (devs[devs.size() / 2 - 1] + devs[devs.size() / 2]);
    double threshold = median_err + k_mad * mad;

    std::vector<Row> candidates;
    for (auto& row : rows) {
        if (row.abs_err_vol_pts > threshold) candidates.push_back(row);
    }

    // Stage 2: economic screen, actual quote-level half-spread charge.
    std::vector<Row> survivors;
    for (auto& row : candidates) {
        if (std::fabs(row.edge_price) > row.half_spread_price) survivors.push_back(row);
    }
    std::sort(survivors.begin(), survivors.end(),
              [](const Row& a, const Row& b) { return std::fabs(a.edge_price) > std::fabs(b.edge_price); });

    std::printf("loaded %zu quotes from %s\n", rows.size(), chainPath.c_str());
    std::printf("stage 1 (materiality, median=%.4f vol pts, MAD=%.4f vol pts, threshold=median+%.1fxMAD=%.4f vol pts): "
                "%zu of %zu quotes are dislocation candidates\n",
                median_err, mad, k_mad, threshold, candidates.size(), rows.size());
    std::printf("stage 2 (economic, per-quote half-spread charge): %zu of %zu candidates survive\n",
                survivors.size(), candidates.size());
    std::printf("\nSUMMARY: %zu of an unfiltered %zu dislocations survived a half-spread charge\n",
                survivors.size(), candidates.size());

    std::printf("\ntop %d survivors by |theoretical edge|:\n", std::min<int>(10, static_cast<int>(survivors.size())));
    std::printf("%-4s %-6s %-8s %-6s %-9s %-9s %-9s %-9s\n", "exp", "T", "K", "type", "resid_vp",
                "edge$", "halfspr$", "mid$");
    for (int i = 0; i < std::min<int>(10, static_cast<int>(survivors.size())); ++i) {
        const auto& row = survivors[static_cast<size_t>(i)];
        std::printf("%-4d %-6.3f %-8.2f %-6c %-9.4f %-9.4f %-9.4f %-9.4f\n", row.expiry_idx, row.T,
                    row.K, row.type, row.fit_vol * 100.0 - row.market_vol * 100.0, row.edge_price,
                    row.half_spread_price, row.mid_price);
    }

    std::ofstream out("docs/screener_survivors.csv");
    out << "expiry_idx,T,K,type,market_vol,fit_vol,resid_vol_pts,edge_price,half_spread_price,mid_price\n";
    for (auto& row : survivors) {
        out << row.expiry_idx << "," << row.T << "," << row.K << "," << row.type << ","
            << row.market_vol << "," << row.fit_vol << "," << (row.fit_vol - row.market_vol) * 100.0
            << "," << row.edge_price << "," << row.half_spread_price << "," << row.mid_price << "\n";
    }
    out.close();
    std::printf("\nsurvivors written to: docs/screener_survivors.csv\n");
    return 0;
}
