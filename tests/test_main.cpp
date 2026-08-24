// Hand-rolled test suite (no test framework dependency). Covers:
//  1. Closed-form Black-Scholes against a known textbook value.
//  2. Put-call parity for the closed form, an algebraic identity.
//  3. MC convergence to the closed form (fast smoke test, loose tolerance).
//  4. Variance reduction actually reduces variance on a single run.
//  5. Barrier in-out parity, exact per-path identity.
//  6. Pathwise vs bump-and-revalue Greeks agree (fast smoke test, loose
//     tolerance; the tight 1e-3 claim is measured by bench_greeks with a
//     much larger path count, see docs/greeks_output.txt).
#include <cmath>
#include <cstdio>
#include <string>

#include "barrier.hpp"
#include "black_scholes.hpp"
#include "mc_engine.hpp"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& name) {
    if (cond) {
        ++g_pass;
        std::printf("[PASS] %s\n", name.c_str());
    } else {
        ++g_fail;
        std::printf("[FAIL] %s\n", name.c_str());
    }
}

static void check_close(double a, double b, double tol, const std::string& name) {
    check(std::fabs(a - b) <= tol, name + " (|" + std::to_string(a) + " - " + std::to_string(b) +
                                        "| <= " + std::to_string(tol) + ")");
}

int main() {
    // 1. Textbook value: S=100,K=100,r=0.05,sigma=0.2,T=1 -> call ~= 10.4506
    {
        bs::BSParams p{100.0, 100.0, 0.05, 0.20, 1.0};
        double c = bs::call_price(p);
        check_close(c, 10.4506, 1e-3, "BS call textbook value");
    }

    // 2. Put-call parity: C - P = S - K e^{-rT}, exact to numerical precision.
    {
        double params[][5] = {
            {100.0, 100.0, 0.05, 0.20, 1.0},
            {80.0, 100.0, 0.03, 0.35, 0.25},
            {120.0, 90.0, 0.01, 0.15, 2.0},
        };
        for (auto& row : params) {
            bs::BSParams p{row[0], row[1], row[2], row[3], row[4]};
            double c = bs::call_price(p);
            double put = bs::put_price(p);
            double rhs = p.S - p.K * std::exp(-p.r * p.T);
            check_close(c - put, rhs, 1e-9, "put-call parity");
        }
    }

    // 3. MC (variance-reduced) converges to closed form, fast smoke test.
    {
        bs::BSParams p{100.0, 100.0, 0.03, 0.20, 1.0};
        double bsPrice = bs::call_price(p);
        mc::MCResult mr =
            mc::price_reduced(mc::OptionType::Call, p.S, p.K, p.r, p.sigma, p.T, 200000, 1);
        double tol = 6.0 * mr.stderr_ + 0.02; // statistical + small floor
        check_close(mr.price, bsPrice, tol, "MC reduced price converges to BS (smoke test)");
    }

    // 4. Variance actually reduced on a single matched run.
    {
        mc::MCResult naive = mc::price_naive(mc::OptionType::Call, 100.0, 100.0, 0.03, 0.20, 1.0,
                                              200000, 55);
        mc::MCResult reduced = mc::price_reduced(mc::OptionType::Call, 100.0, 100.0, 0.03, 0.20,
                                                  1.0, 200000, 55);
        check(reduced.stderr_ < naive.stderr_, "antithetic+CV standard error < naive");
    }

    // 5. Barrier in-out parity: exact per-path identity on shared paths.
    {
        barrier::BarrierResult res = barrier::price_down_barrier(
            mc::OptionType::Call, 100.0, 100.0, 85.0, 0.03, 0.20, 1.0, 5000, 100, 99);
        double sum = res.down_out_price + res.down_in_price;
        check_close(sum, res.vanilla_shared_price, 1e-9, "barrier in-out exact parity (shared paths)");
    }

    // 6. Pathwise vs bump-and-revalue Greeks, fast smoke test (loose tol).
    {
        long N = 300000;
        mc::PathwiseGreeks pw =
            mc::pathwise_greeks(mc::OptionType::Call, 100.0, 100.0, 0.03, 0.20, 1.0, N, 321);
        mc::PathwiseGreeks br = mc::bump_reval_greeks(mc::OptionType::Call, 100.0, 100.0, 0.03,
                                                       0.20, 1.0, N, 321, 0.5, 0.005);
        check_close(pw.delta, br.delta, 0.05, "pathwise vs bump delta (smoke test)");
        check_close(pw.vega, br.vega, 0.5, "pathwise vs bump vega (smoke test)");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
