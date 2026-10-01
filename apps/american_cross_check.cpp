// Generates a shared set of American-option test cases and prices every one
// with the CRR tree in american_tree.hpp, writing a CSV. The independent
// oracle, python/validate_american_tree.py, reprices the exact same cases
// with a from-scratch, obviously-correct (slow) pure-Python CRR
// implementation (oracle_american_tree.py) and diffs the two exactly, the
// same two-language cross-check pattern this repo already uses for
// black_scholes.hpp vs black_scholes_ref.py.
//
// Usage: ./american_cross_check <steps> <out.csv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "american_tree.hpp"

struct Case {
    double S0, K, r, borrow, sigma, T;
    bool is_put;
    std::vector<crr::DiscreteDividend> divs;
};

int main(int argc, char** argv) {
    int steps = argc > 1 ? std::atoi(argv[1]) : 200;
    std::string out_path = argc > 2 ? argv[2] : "docs/american_cross_check.csv";

    std::vector<Case> cases;
    // Spans: ITM/ATM/OTM, short/long maturity, zero/one/two dividends,
    // zero/nonzero borrow, calls and puts.
    std::vector<double> spots = {80.0, 95.0, 100.0, 105.0, 120.0};
    std::vector<double> strikes = {90.0, 100.0, 110.0};
    std::vector<double> sigmas = {0.15, 0.25, 0.40};
    std::vector<double> maturities = {0.25, 1.0, 1.5};
    std::vector<double> borrows = {0.0, 0.015};
    int divs_pattern = 0;
    for (double S0 : spots) {
        for (double K : strikes) {
            for (double sigma : sigmas) {
                for (double T : maturities) {
                    double borrow = borrows[static_cast<size_t>(divs_pattern) % borrows.size()];
                    std::vector<crr::DiscreteDividend> divs;
                    int pattern = divs_pattern % 3;
                    if (pattern == 1) {
                        divs.push_back({0.5 * T, 0.03 * S0});
                    } else if (pattern == 2) {
                        divs.push_back({0.3 * T, 0.02 * S0});
                        divs.push_back({0.7 * T, 0.02 * S0});
                    }
                    bool is_put = (divs_pattern % 2) == 0;
                    cases.push_back({S0, K, 0.03, borrow, sigma, T, is_put, divs});
                    ++divs_pattern;
                }
            }
        }
    }

    std::ofstream out(out_path);
    if (!out.is_open()) {
        std::fprintf(stderr, "failed to open %s for writing (does its parent directory exist?)\n",
                      out_path.c_str());
        return 1;
    }
    out << "case_id,S0,K,r,borrow,sigma,T,is_put,steps,n_divs,div1_time,div1_amt,div2_time,div2_amt,"
           "price\n";
    crr::Workspace ws(steps);
    int id = 0;
    for (auto& c : cases) {
        crr::TreeParams p{c.S0, c.K, c.r, c.borrow, c.sigma, c.T, steps, &c.divs};
        double price = ws.price(p, !c.is_put, crr::Exercise::American);
        double d1t = c.divs.size() > 0 ? c.divs[0].time : 0.0;
        double d1a = c.divs.size() > 0 ? c.divs[0].amount : 0.0;
        double d2t = c.divs.size() > 1 ? c.divs[1].time : 0.0;
        double d2a = c.divs.size() > 1 ? c.divs[1].amount : 0.0;
        out << id << "," << c.S0 << "," << c.K << "," << c.r << "," << c.borrow << "," << c.sigma
            << "," << c.T << "," << (c.is_put ? 1 : 0) << "," << steps << "," << c.divs.size()
            << "," << d1t << "," << d1a << "," << d2t << "," << d2a << "," << price << "\n";
        ++id;
    }
    out.close();
    std::printf("wrote %d cases (steps=%d) to %s\n", id, steps, out_path.c_str());
    return 0;
}
