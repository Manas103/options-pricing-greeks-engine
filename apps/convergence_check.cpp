// Justifies the 40-60 step count used by vol_store_invert for the full 12M-
// quote batch inversion: prices a handful of representative American cases
// at increasing step counts and shows where the price stops moving, against
// a 1000-step tree as the practical "true" reference.
//
// Usage: ./convergence_check
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "american_tree.hpp"

struct Case {
    std::string name;
    double S0, K, r, borrow, sigma, T;
    bool is_put;
    std::vector<crr::DiscreteDividend> divs;
};

int main() {
    std::vector<Case> cases = {
        {"ATM put, 1y, 2 divs", 100.0, 100.0, 0.03, 0.01, 0.25, 1.0, true,
         {{0.3, 2.0}, {0.8, 2.0}}},
        {"25-delta-ish OTM put, 1y, high div", 100.0, 88.0, 0.03, 0.012, 0.25, 1.0, true,
         {{0.3, 3.5}, {0.8, 3.5}}},
        {"ITM call, 0.5y, 1 div", 100.0, 90.0, 0.03, 0.005, 0.20, 0.5, false, {{0.25, 1.5}}},
        {"deep OTM put, 1.5y, no div", 100.0, 70.0, 0.03, 0.0, 0.35, 1.5, true, {}},
    };

    std::vector<int> step_counts = {10, 20, 30, 40, 50, 60, 80, 100, 150, 200, 400, 1000};

    for (auto& c : cases) {
        std::printf("\n=== %s (S0=%.1f K=%.1f sigma=%.2f T=%.2f borrow=%.3f divs=%zu) ===\n",
                    c.name.c_str(), c.S0, c.K, c.sigma, c.T, c.borrow, c.divs.size());
        crr::Workspace ws(1000);
        double reference = 0.0;
        for (int steps : step_counts) {
            crr::TreeParams p{c.S0, c.K, c.r, c.borrow, c.sigma, c.T, steps, &c.divs};
            auto t0 = std::chrono::high_resolution_clock::now();
            double price = ws.price(p, !c.is_put, crr::Exercise::American);
            auto t1 = std::chrono::high_resolution_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (steps == 1000) reference = price;
            std::printf("  steps=%4d  price=%.6f  time=%8.2f us\n", steps, price, us);
        }
        std::printf("  (reference @ 1000 steps = %.6f)\n", reference);
        // Re-report the diff at 40-60 explicitly, since that is the claimed
        // operating range for the batch inversion.
        for (int steps : {40, 50, 60}) {
            crr::TreeParams p{c.S0, c.K, c.r, c.borrow, c.sigma, c.T, steps, &c.divs};
            double price = ws.price(p, !c.is_put, crr::Exercise::American);
            std::printf("  diff vs 1000-step reference at steps=%d: %.6f (%.4f%%)\n", steps,
                        price - reference, 100.0 * (price - reference) / reference);
        }
    }
    return 0;
}
