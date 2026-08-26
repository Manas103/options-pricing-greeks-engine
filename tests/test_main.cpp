// Hand-rolled test suite (no test framework dependency). Covers:
//  1. Closed-form Black-Scholes against a known textbook value.
//  2. Put-call parity for the closed form, an algebraic identity.
//  3. MC convergence to the closed form (fast smoke test, loose tolerance).
//  4. Variance reduction actually reduces variance on a single run.
//  5. Barrier in-out parity, exact per-path identity.
//  6. Pathwise vs bump-and-revalue Greeks agree (fast smoke test, loose
//     tolerance; the tight 1e-3 claim is measured by bench_greeks with a
//     much larger path count, see docs/greeks_output.txt).
//  7-8. Implied vol solver round-trips a known vol for a call and a put.
//  9. is_convex accepts a hand-picked SVI slice known to be convex.
//  10. is_convex REJECTS a hand-picked SVI slice with a manufactured
//      butterfly-arbitrage dip (a negative control, not just a positive one).
//  11. respects_calendar_floor rejects a later slice that dips below an
//      earlier slice's total variance.
//  12. respects_calendar_floor accepts a later slice that stays above.
//  13. fit_slice on noiseless, SVI-generated data recovers a near-zero SSE
//      feasible fit (a reference-oracle-style test: the fit is checked
//      against data whose exact answer is known by construction).
//  14. fit_slice's calendar constraint actually binds: fitting a second,
//      artificially-lowered slice against a floor from the first still
//      returns a feasible result that respects the floor, not the
//      unconstrained (lower) optimum.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "barrier.hpp"
#include "black_scholes.hpp"
#include "implied_vol.hpp"
#include "mc_engine.hpp"
#include "svi.hpp"

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

    // 7-8. Implied vol solver round-trips a known vol.
    {
        bs::BSParams p{100.0, 105.0, 0.025, 0.27, 0.75};
        double callPrice = bs::call_price(p);
        ivol::ImpliedVolResult iv = ivol::solve(mc::OptionType::Call, callPrice, p.S, p.K, p.r, p.T);
        check(iv.converged, "implied vol solver converges (call)");
        check_close(iv.vol, p.sigma, 1e-6, "implied vol solver round-trips known vol (call)");
    }
    {
        bs::BSParams p{100.0, 92.0, 0.025, 0.31, 0.4};
        double putPrice = bs::put_price(p);
        ivol::ImpliedVolResult iv = ivol::solve(mc::OptionType::Put, putPrice, p.S, p.K, p.r, p.T);
        check(iv.converged, "implied vol solver converges (put)");
        check_close(iv.vol, p.sigma, 1e-6, "implied vol solver round-trips known vol (put)");
    }

    // 9. is_convex accepts a hand-picked convex SVI slice.
    {
        svi::Params p{0.04, 0.10, -0.30, 0.0, 0.10};
        std::vector<double> grid;
        for (int i = -20; i <= 20; ++i) grid.push_back(i * 0.02);
        check(svi::is_convex(p, grid), "is_convex accepts a genuine SVI slice");
    }

    // 10. is_convex rejects a genuine violation: raw SVI's second derivative
    // in k, b*sigma^2/((k-m)^2+sigma^2)^1.5, is non-negative for any b >= 0,
    // so a raw SVI slice with b >= 0 is ALWAYS convex in log-moneyness; the
    // only way is_convex can fail on a valid-shaped slice is the OTHER half
    // of the check, total variance dipping negative. Built by hand: at
    // k = m the minimum of w is a + b*sigma*sqrt(1-rho^2); with a very
    // negative and rho = 0 that minimum is forced below zero.
    {
        svi::Params negative_min{-0.05, 0.05, 0.0, 0.0, 0.05}; // w(0) = -0.05 + 0.05*0.05 = -0.0475
        std::vector<double> grid;
        for (int i = -10; i <= 10; ++i) grid.push_back(i * 0.02);
        check(!svi::is_convex(negative_min, grid),
              "is_convex rejects a slice whose total variance dips negative");
    }

    // 11. respects_calendar_floor rejects a later slice that dips below an
    // earlier slice's total variance at some k (real violation, built by
    // hand: floor is a flat 0.05, candidate is a flat 0.03).
    {
        std::vector<double> grid = {-0.2, -0.1, 0.0, 0.1, 0.2};
        std::vector<double> floor_w(grid.size(), 0.05);
        svi::Params low{0.03, 0.0, 0.0, 0.0, 0.10}; // b=0: flat total variance = a = 0.03 everywhere
        check(!svi::respects_calendar_floor(low, grid, floor_w),
              "respects_calendar_floor rejects a genuine violation");
    }

    // 12. respects_calendar_floor accepts a later slice that clears the floor.
    {
        std::vector<double> grid = {-0.2, -0.1, 0.0, 0.1, 0.2};
        std::vector<double> floor_w(grid.size(), 0.05);
        svi::Params high{0.07, 0.0, 0.0, 0.0, 0.10}; // flat total variance = 0.07 > 0.05 everywhere
        check(svi::respects_calendar_floor(high, grid, floor_w),
              "respects_calendar_floor accepts a slice that clears the floor");
    }

    // 13. fit_slice recovers noiseless, SVI-generated data (reference-
    // oracle style: the exact answer is known because the data was
    // generated from a known SVI slice with zero noise added).
    {
        svi::Params truth{0.05, 0.20, -0.35, 0.02, 0.18};
        std::vector<double> grid;
        for (int i = -20; i <= 20; ++i) grid.push_back(i * 0.02);
        std::vector<svi::FitPoint> pts;
        for (double k : grid) pts.push_back({k, svi::total_variance(truth, k), 1.0});
        svi::Params init{0.04, 0.10, -0.10, 0.0, 0.25};
        svi::FitResult fr = svi::fit_slice(pts, grid, init);
        check(fr.feasible, "fit_slice returns a feasible fit on noiseless SVI-generated data");
        check(fr.sse < 1e-6, "fit_slice recovers noiseless SVI-generated data to near-zero SSE");
    }

    // 14. fit_slice's calendar constraint actually binds: fit a slice whose
    // unconstrained optimum would sit BELOW a floor built from a prior,
    // higher-level slice, and check the returned slice still respects it.
    {
        svi::Params priorTruth{0.08, 0.10, -0.20, 0.0, 0.20}; // higher level than the "true" second slice
        svi::Params secondTruth{0.03, 0.15, -0.30, 0.0, 0.15}; // would-be unconstrained optimum, lower
        std::vector<double> grid;
        for (int i = -20; i <= 20; ++i) grid.push_back(i * 0.02);
        std::vector<double> floor_w;
        for (double k : grid) floor_w.push_back(svi::total_variance(priorTruth, k));
        std::vector<svi::FitPoint> pts;
        for (double k : grid) pts.push_back({k, svi::total_variance(secondTruth, k), 1.0});
        svi::Params init = priorTruth;
        init.a += 0.001;
        svi::FitResult fr = svi::fit_slice(pts, grid, init, &grid, &floor_w);
        check(fr.feasible, "fit_slice with an active calendar floor still returns feasible");
        check(svi::respects_calendar_floor(fr.params, grid, floor_w),
              "fit_slice's returned params respect the calendar floor directly");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
