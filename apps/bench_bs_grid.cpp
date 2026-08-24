// Sweeps a strike x maturity grid, prices each cell with the variance-
// reduced Monte Carlo engine, and compares against the closed-form
// Black-Scholes price. Reports max/mean relative error and writes a CSV.
//
// Usage: bench_bs_grid <n_strikes> <n_maturities> <paths_per_cell> <seed> <out_csv>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

#include "black_scholes.hpp"
#include "mc_engine.hpp"

struct Cell {
    double K, T, bs_price, mc_price, mc_stderr, rel_err_pct;
};

int main(int argc, char** argv) {
    int nK = (argc > 1) ? std::atoi(argv[1]) : 20;
    int nT = (argc > 2) ? std::atoi(argv[2]) : 20;
    long N = (argc > 3) ? std::atol(argv[3]) : 2000000L;
    unsigned long baseSeed = (argc > 4) ? std::strtoul(argv[4], nullptr, 10) : 42UL;
    std::string outCsv = (argc > 5) ? argv[5] : "docs/bs_grid.csv";

    const double S0 = 100.0, r = 0.03, sigma = 0.20;
    // Moneyness grid 85%-115%, maturities 0.25-2.0y. An earlier attempt used
    // 80%-120% moneyness down to T=0.1y; several of those cells price under
    // 1 cent (e.g. K=120, T=0.1y is worth $0.0047), where relative error is
    // dominated by the MC absolute noise floor rather than by pricing bias.
    // See README "Findings" for the measurement that established this and
    // "Measured results" for the honest max/mean error on the wider grid.
    std::vector<double> strikes(nK), maturities(nT);
    for (int i = 0; i < nK; ++i) {
        double frac = (nK == 1) ? 0.0 : static_cast<double>(i) / (nK - 1);
        strikes[i] = S0 * (0.85 + 0.30 * frac);
    }
    for (int j = 0; j < nT; ++j) {
        double frac = (nT == 1) ? 0.0 : static_cast<double>(j) / (nT - 1);
        maturities[j] = 0.25 + (2.00 - 0.25) * frac;
    }

    std::vector<Cell> cells(static_cast<size_t>(nK) * nT);
    unsigned nThreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    std::vector<size_t> idx(cells.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;

    auto worker = [&](unsigned tid) {
        for (size_t c = tid; c < cells.size(); c += nThreads) {
            int i = static_cast<int>(c / nT);
            int j = static_cast<int>(c % nT);
            double K = strikes[i], T = maturities[j];
            bs::BSParams p{S0, K, r, sigma, T};
            double bsPrice = bs::call_price(p);
            unsigned long seed = baseSeed + 1000003UL * static_cast<unsigned long>(c);
            mc::MCResult mr = mc::price_reduced(mc::OptionType::Call, S0, K, r, sigma, T, N, seed);
            double relErr = (bsPrice > 1e-8) ? std::fabs(mr.price - bsPrice) / bsPrice * 100.0 : 0.0;
            cells[c] = {K, T, bsPrice, mr.price, mr.stderr_, relErr};
        }
    };
    for (unsigned t = 0; t < nThreads; ++t) pool.emplace_back(worker, t);
    for (auto& th : pool) th.join();

    std::ofstream csv(outCsv);
    csv << "K,T,bs_price,mc_price,mc_stderr,rel_err_pct\n";
    double maxErr = 0.0, sumErr = 0.0;
    for (auto& c : cells) {
        csv << c.K << "," << c.T << "," << c.bs_price << "," << c.mc_price << "," << c.mc_stderr
            << "," << c.rel_err_pct << "\n";
        maxErr = std::max(maxErr, c.rel_err_pct);
        sumErr += c.rel_err_pct;
    }
    csv.close();

    double meanErr = sumErr / static_cast<double>(cells.size());
    std::printf("grid: %d strikes x %d maturities = %zu cells, N=%ld paths/cell, threads=%u\n", nK,
                nT, cells.size(), N, nThreads);
    std::printf("max relative error: %.5f %%\n", maxErr);
    std::printf("mean relative error: %.5f %%\n", meanErr);
    std::printf("csv written to: %s\n", outCsv.c_str());
    return 0;
}
