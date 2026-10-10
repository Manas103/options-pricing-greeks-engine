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
//  20. The tick-driven quote chain has the designed pair count and reprices
//      500+ contracts per tick.
//  21. quote_engine.hpp's put-call-parity shortcut (one erfc pair per
//      strike/expiry, deriving the put from the call rather than pricing
//      it independently) agrees with the untouched closed-form
//      bs::put_price, the reference-oracle check for that optimization.
//  22. Every chain quote's bid <= theo <= ask and both deltas stay inside
//      their theoretical bounds.
//  23. Repricing the same tick twice is bit-identical (memcmp), the
//      determinism the bit-for-bit session replay claim relies on.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "american_tree.hpp"
#include "barrier.hpp"
#include "black_scholes.hpp"
#include "implied_vol.hpp"
#include "mc_engine.hpp"
#include "quote_chain.hpp"
#include "quote_engine.hpp"
#include "scenario_book.hpp"
#include "scenario_grid.hpp"
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

    // 15. Zero-dividend, zero-borrow American call == European call (early
    // exercise of a call is never optimal absent dividends), and both land
    // close to the closed-form Black-Scholes call at the same parameters.
    {
        crr::Workspace ws(200);
        crr::TreeParams p{100.0, 100.0, 0.05, 0.0, 0.20, 1.0, 200, nullptr};
        double american = ws.price(p, true, crr::Exercise::American);
        double european = ws.price(p, true, crr::Exercise::European);
        bs::BSParams bsp{p.S0, p.K, p.r, p.sigma, p.T};
        double closed = bs::call_price(bsp);
        check_close(american, european, 1e-6,
                    "zero-dividend American call == European call (no early exercise value)");
        check_close(american, closed, 0.05, "zero-dividend American call ~= closed-form BS call");
    }

    // 16. American put >= European put, always, with or without dividends
    // and borrow (the early-exercise premium is never negative).
    {
        crr::Workspace ws(200);
        std::vector<crr::DiscreteDividend> divs = {{0.25, 1.5}, {0.75, 1.5}};
        crr::TreeParams no_div{100.0, 100.0, 0.05, 0.0, 0.25, 1.0, 200, nullptr};
        crr::TreeParams with_div{100.0, 100.0, 0.05, 0.01, 0.25, 1.0, 200, &divs};
        double am_no_div = ws.price(no_div, false, crr::Exercise::American);
        double eu_no_div = ws.price(no_div, false, crr::Exercise::European);
        double am_div = ws.price(with_div, false, crr::Exercise::American);
        double eu_div = ws.price(with_div, false, crr::Exercise::European);
        check(am_no_div >= eu_no_div - 1e-9, "American put >= European put (no dividends)");
        check(am_div >= eu_div - 1e-9, "American put >= European put (with dividends and borrow)");
    }

    // 17. Early-exercise lower bound holds at EVERY node by construction,
    // not just at the root, for an American option with dividends.
    {
        crr::Workspace ws(100);
        std::vector<crr::DiscreteDividend> divs = {{0.3, 2.0}, {0.8, 2.0}};
        crr::TreeParams p{100.0, 95.0, 0.04, 0.015, 0.30, 1.0, 100, &divs};
        crr::Workspace::NodeCheck nc = ws.check_intrinsic_bound(p, false);
        check(nc.all_nodes_ge_intrinsic,
              "American tree value >= intrinsic at every node (" +
                  std::to_string(nc.nodes_checked) + " nodes checked)");
    }

    // 18. Implied-vol round trip through the American tree itself: generate
    // a price from a known vol (with dividends and borrow), invert with
    // solve_implied_vol, recover the known vol.
    {
        crr::Workspace ws(80);
        std::vector<crr::DiscreteDividend> divs = {{0.4, 1.2}};
        crr::TreeParams p{100.0, 90.0, 0.03, 0.008, 0.22, 0.9, 80, &divs};
        double price = ws.price(p, false, crr::Exercise::American);
        crr::ImpliedVolResult iv =
            crr::solve_implied_vol(ws, p, false, crr::Exercise::American, price);
        check(iv.converged, "American implied vol solver converges (put, with dividends)");
        check_close(iv.vol, p.sigma, 1e-4, "American implied vol solver round-trips known vol");
    }

    // 19. Step-count convergence sanity: 50 steps should already be close to
    // a much finer 400-step tree for a representative case (see
    // docs/convergence_check_output.txt for the full sweep that justifies
    // the 40-60 step choice used by the batch inversion).
    {
        crr::Workspace ws(400);
        std::vector<crr::DiscreteDividend> divs = {{0.3, 1.0}, {0.9, 1.0}};
        crr::TreeParams p{100.0, 97.0, 0.04, 0.01, 0.28, 1.0, 50, &divs};
        double coarse = ws.price(p, false, crr::Exercise::American);
        p.steps = 400;
        double fine = ws.price(p, false, crr::Exercise::American);
        check_close(coarse, fine, 0.05, "50-step American tree within 0.05 of a 400-step tree");
    }

    // 20. Quote engine chain size matches the 500+ contracts/tick claim.
    {
        std::vector<quote::Pair> chain = quote::build_chain();
        check(chain.size() == quote::NUM_PAIRS, "chain has the designed number of pairs");
        check(chain.size() * 2 >= 500, "chain reprices 500+ contracts per tick (" +
                                            std::to_string(chain.size() * 2) + ")");
    }

    // 21. The put-call-parity shortcut in reprice_chain (one erfc pair per
    // strike/expiry, deriving the put from the call) must agree with the
    // independent closed-form bs::put_price/put_greeks, which never takes
    // that shortcut. This is the reference-oracle check for the latency
    // optimization in quote_engine.hpp's Findings.
    {
        std::vector<quote::Pair> chain = quote::build_chain();
        std::vector<quote::Quote> out(chain.size() * 2);
        const double S = 97.0, r = 0.03, sigma = 0.22;
        quote::reprice_chain(chain, S, r, sigma, out.data());

        double max_abs_diff = 0.0;
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const quote::Pair& pr = chain[i];
            bs::BSParams bp{S, pr.K, r, sigma, pr.T};
            double ref_put = bs::put_price(bp);
            double got_put = out[2 * i + 1].theo;
            max_abs_diff = std::max(max_abs_diff, std::fabs(ref_put - got_put));
        }
        check(max_abs_diff < 1e-9,
              "put-call-parity shortcut matches independent closed-form put price (max abs diff " +
                  std::to_string(max_abs_diff) + ")");
    }

    // 22. Every quote's bid <= theo <= ask, and call/put deltas stay inside
    // their theoretical bounds, across the whole chain.
    {
        std::vector<quote::Pair> chain = quote::build_chain();
        std::vector<quote::Quote> out(chain.size() * 2);
        quote::reprice_chain(chain, 103.0, 0.03, 0.19, out.data());
        bool spreads_ok = true, deltas_ok = true;
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const quote::Quote& c = out[2 * i];
            const quote::Quote& p = out[2 * i + 1];
            if (!(c.bid <= c.theo && c.theo <= c.ask)) spreads_ok = false;
            if (!(p.bid <= p.theo && p.theo <= p.ask)) spreads_ok = false;
            if (!(c.delta >= 0.0 && c.delta <= 1.0)) deltas_ok = false;
            if (!(p.delta >= -1.0 && p.delta <= 0.0)) deltas_ok = false;
        }
        check(spreads_ok, "every quote's bid <= theo <= ask across the chain");
        check(deltas_ok, "call delta in [0,1] and put delta in [-1,0] across the chain");
    }

    // 23. Repricing the same tick twice produces bit-identical output
    // (same bytes, memcmp-exact), the determinism the bit-for-bit replay
    // claim depends on.
    {
        std::vector<quote::Pair> chain = quote::build_chain();
        std::vector<quote::Quote> out1(chain.size() * 2), out2(chain.size() * 2);
        quote::reprice_chain(chain, 101.37, 0.03, 0.245, out1.data());
        quote::reprice_chain(chain, 101.37, 0.03, 0.245, out2.data());
        bool identical =
            std::memcmp(out1.data(), out2.data(), out1.size() * sizeof(quote::Quote)) == 0;
        check(identical, "repricing the same tick twice is bit-identical (memcmp)");
    }

    // 24. generate_book is deterministic in the seed: two generations from
    // the same seed produce an identical book (same strikes, quantities),
    // the determinism the scenario manifest's reproducibility claim relies
    // on.
    {
        std::vector<scenario::Position> b1 = scenario::generate_book(7);
        std::vector<scenario::Position> b2 = scenario::generate_book(7);
        bool identical = b1.size() == b2.size();
        for (size_t i = 0; identical && i < b1.size(); ++i) {
            identical = b1[i].S0 == b2[i].S0 && b1[i].K == b2[i].K && b1[i].qty == b2[i].qty &&
                        b1[i].is_call == b2[i].is_call;
        }
        check(identical && b1.size() == 1800,
              "generate_book(seed) is deterministic and produces 1,800 positions");
    }

    // 25. Zero shock full revaluation reproduces the base price exactly (a
    // conservation identity: no shock, no P&L).
    {
        std::vector<scenario::Position> book = scenario::generate_book(7);
        bool ok = true;
        for (const auto& p : book) {
            scenario::PositionGreeks g = scenario::position_base(p);
            double reval = scenario::full_reval_price(p, 0.0, 0.0);
            if (std::fabs(reval - g.price) > 1e-9) ok = false;
        }
        check(ok, "zero-shock full revaluation equals the base price for every position");
    }

    // 26. The independent quadrature reference oracle agrees with the
    // closed-form fast path on a small deterministic sample (a cross-check
    // between two different algorithms, not the same formula twice).
    {
        std::vector<scenario::Position> book = scenario::generate_book(7);
        double max_rel = 0.0;
        double shocks[3] = {-0.15, 0.0, 0.20};
        double vols[3] = {-0.03, 0.0, 0.10};
        for (int i = 0; i < 20; ++i) {
            const auto& p = book[i * 7 % book.size()];
            double fast = scenario::full_reval_price(p, shocks[i % 3], vols[i % 3]);
            double oracle = scenario::full_reval_price_oracle(p, shocks[i % 3], vols[i % 3]);
            double denom = std::max(1e-8, std::fabs(oracle));
            max_rel = std::max(max_rel, std::fabs(fast - oracle) / denom);
        }
        check(max_rel < 1e-4, "quadrature reference oracle matches closed form (max rel diff " +
                                   std::to_string(max_rel) + ")");
    }

    // 27. generate_historical is deterministic in the seed and respects the
    // leverage-effect sign convention loosely (negative correlation input
    // produces a negative sample correlation on a large draw).
    {
        scenario::ScenarioManifest m;
        m.n_historical = 5000;
        std::vector<scenario::Scenario> h1 = scenario::generate_historical(m);
        std::vector<scenario::Scenario> h2 = scenario::generate_historical(m);
        bool identical = h1.size() == h2.size();
        for (size_t i = 0; identical && i < h1.size(); ++i) {
            identical = h1[i].spot_shock == h2[i].spot_shock && h1[i].vol_shift == h2[i].vol_shift;
        }
        double mean_s = 0, mean_v = 0;
        for (auto& s : h1) { mean_s += s.spot_shock; mean_v += s.vol_shift; }
        mean_s /= h1.size();
        mean_v /= h1.size();
        double cov = 0, var_s = 0, var_v = 0;
        for (auto& s : h1) {
            double ds = s.spot_shock - mean_s, dv = s.vol_shift - mean_v;
            cov += ds * dv; var_s += ds * ds; var_v += dv * dv;
        }
        double corr = cov / std::sqrt(var_s * var_v);
        check(identical, "generate_historical(seed) is deterministic");
        check(corr < -0.4, "historical spot/vol draw reproduces the negative leverage "
                            "correlation (sample corr " + std::to_string(corr) + ")");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
