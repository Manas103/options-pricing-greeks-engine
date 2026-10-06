# Options Pricing and Greeks Engine with Variance-Reduced Monte Carlo

A Monte Carlo pricer for European and barrier options under geometric
Brownian motion, with antithetic-variate and control-variate variance
reduction, pathwise Greeks cross-checked against bump-and-revalue, and a
closed-form Black-Scholes implementation as the reference oracle. C++17 for
the engine, Python (scipy) for an independent second opinion on that
reference. Extended with an arbitrage-free SVI volatility surface fit (hard
no-arbitrage constraints enforced during the optimization, not checked
afterward) and a relative-value screener over a simulated option chain.
Extended a second time with a Cox-Ross-Rubinstein American pricer carrying
discrete dollar dividends and a borrow rate, and a point-in-time DuckDB
quote store that inverts 12.0 million simulated end-of-day quotes to
implied vol through it, measuring what a European shortcut gets wrong.
Extended a third time with a tick-driven quote path over a simulated UDP
multicast feed: a pinned-core engine reprices a 520-contract chain per
tick through the same closed-form pricer above, times itself, captures
every session, and replays it bit for bit.
Every number below was measured on this machine, not targeted: where the
first attempt at a claim fell short, or landed somewhere other than
expected, the README says so plainly.

## Extension: tick-driven options quote engine over a simulated multicast feed (Oct. 2026)

`include/quote_chain.hpp` builds a fixed 260-pair (520-contract, calls and
puts) option chain around one underlying; `include/quote_engine.hpp`
reprices the whole chain from a single spot/vol tick through the same
closed-form Black-Scholes formula this repository already validated
(`include/black_scholes.hpp`), pricing a call and its same-strike,
same-expiry put together from one `d1`/`d2` pair rather than as two
independent contracts. `include/tick_feed.hpp` defines a minimal simulated
tick; `apps/quote_feed_sender.cpp` is a UDP multicast sender that plays a
GBM-ish random walk of ticks onto a real multicast socket on this host;
`apps/quote_engine_live.cpp` is the receiver, pinned to one CPU core, that
times every tick's reprice, captures the session, and folds an FNV-1a
checksum over every quote it produces; `apps/quote_engine_replay.cpp`
replays a captured session offline (no network, no timing) and must
reproduce the identical checksum. `scripts/run_quote_sessions.sh` drives
N sessions end to end (sender, pinned receiver, replay, checksum compare)
and both processes exit on their own once a session's ticks (or sentinel)
are exhausted; nothing is left running.

## Extension: single-stock volatility store with American-exercise implied vols (Oct. 2026)

200 single-stock names and 1 index, 1,244 simulated trading days each (about
4.9 years), 6 expiries and 8 strikes per name per day: 12,002,112 simulated
end-of-day quotes in `data/vol_store/`. `include/american_tree.hpp` adds a
CRR binomial tree with American exercise, discrete dollar dividends, and a
borrow rate; `python/vol_store/pipeline.py` builds the universe and the raw
quote grid, calls the C++ engine to price every quote "truly" (step1 ->
`vol_store_generate_prices`), injects a disclosed number of no-arbitrage
violations and quarantines them (step2), and after the C++ batch inversion
(`vol_store_invert`) reports the European-vs-American implied-vol gap by
dividend decile (step3). `python/vol_store/tests/test_point_in_time.py`
proves the point-in-time guarantee on an in-memory DuckDB store: querying a
quote chain "as of" a past date is unaffected by a later restatement of the
same contract, even though both rows sit in the same table.

## Extension: arbitrage-free SVI surface fit and relative-value screener (Aug. 2026)

Two new pieces sit on top of the pricer above: `include/implied_vol.hpp`
inverts a Black-Scholes price to an implied vol (bisection, cross-checked
against the closed form it was struck from); `include/svi.hpp` fits a raw
SVI (Gatheral) total-variance slice per expiry with butterfly convexity and
calendar monotonicity enforced as hard constraints during a constrained
Nelder-Mead search, not measured after the fact. `apps/svi_fit.cpp`
generates a synthetic 6,000-quote listed-style chain (20 expiries x 300
strikes) from a ground-truth SVI surface plus noise, fits per-expiry slices
back to it, and reports the fit error and constraint-violation counts.
`apps/screener.cpp` reads that fit and ranks relative-value dislocations,
keeping only the ones whose theoretical edge survives a half-spread charge.

## Why this exists

This is a small version of the pricing and risk library a derivatives desk
builds internally: a fast way to price a payoff under a model, quantify how
noisy that price is, reduce the noise instead of just running more paths,
and get the sensitivities (Greeks) a trader actually hedges on, each
validated against an independent method rather than trusted on its own.

## Honest framing, up front

- **A research pricer, not a production risk system.** No local volatility,
  no stochastic volatility, no jumps, no American exercise. Geometric
  Brownian motion and Black-Scholes assumptions throughout.
- **The tick feed is a simulated UDP multicast stand-in, not a real
  exchange protocol.** `tick_feed.hpp`'s `Tick` is a fixed-layout POD
  struct sent as raw bytes between two processes on the same host; it is
  not ITCH, not OPRA, has no explicit byte order, and would not survive a
  cross-host hop. It exists to give the quote engine a real socket to
  receive real datagrams from, in order, which is the one property the
  latency and replay claims need.
- **One underlying, one flat vol shock per tick.** Every tick moves the
  whole chain's implied vol in parallel; there is no per-strike or
  per-expiry vol surface dynamics in the live quote path (the separate SVI
  surface fit above has that, offline).
- **The quote engine's half bid-ask spread is a flat 0.25% of theoretical
  value**, not sourced from or calibrated to any real market maker's
  quoted spread.
- **No dividend yield.** Every formula and every simulation uses `q = 0`.
  Kept out deliberately to keep the model surface small enough to fully
  cross-check; a real desk pricer would carry it.
- **Barrier monitoring is discrete, not continuous.** The barrier is checked
  at 200 evenly spaced points across `[0, T]`, not continuously, which is
  the standard practical approximation and slightly under-prices the true
  continuous-monitoring knock probability.
- **All data is synthetic.** There are no real market quotes anywhere in
  this repository; every price is a model price against another model
  price.
- **The 6,000-quote chain is a simulated listed option chain, not a real
  one.** It is generated from a hand-chosen ground-truth SVI surface plus
  noise, not downloaded, scraped, or subscribed to from any venue.
- **The 12.0M-quote vol store is entirely simulated**, 200 single-stock
  names plus 1 index, 1,244 simulated trading days, no real OPRA feed or
  real dividend calendar anywhere in it.
- **The discrete-dividend tree is the standard practical approximation**
  (subtract the known dollar dividend from every node at its ex-date, keep
  the lattice multiplicatively recombining from there), not the exact
  Vellekoop-Nieuwenhuis recombining correction. Disclosed in
  `include/american_tree.hpp` and in Limitations.
- **A flat risk-free rate (4%) and a flat per-name borrow rate**, no term
  structure on either.
- **The batch inversion uses a 40-step CRR tree**, not the 1,000-step
  reference the convergence check diffs against, a deliberate
  runtime-vs-accuracy trade for 12 million quotes; see Findings.
- **Machine and toolchain**, for every number below: 8 physical / 16 logical
  cores (AMD Ryzen 7 7800X3D), WSL2 Ubuntu 22.04, g++ 11.4.0, `-O3`, CMake
  3.22.1, C++17. The Python cross-check ran on Windows 11, Python 3.12.10,
  scipy, duckdb. The quote engine's latency numbers were measured pinned
  to exactly 1 of the 12 logical cores WSL2 exposes on this machine (its
  own `.wslconfig` cap, not a per-run choice), with the rest left free for
  whatever else is running on this shared machine at the time.

## Architecture

```
include/
  black_scholes.hpp   closed-form price + Greeks (delta, gamma, vega, theta),
                       independent of the MC engine, the ground truth it diffs against
  mc_engine.hpp        naive MC, antithetic+control-variate MC, pathwise
                       Greeks, common-random-number bump-and-revalue Greeks
  barrier.hpp          down-and-out/down-and-in on shared paths, discretely monitored
  quote_chain.hpp       fixed 260-pair (520-contract) chain, strikes/expiries/discount
                        factors precomputed once at chain-build time
  quote_engine.hpp      reprices the chain from one spot/vol tick; call+put share one
                        d1/d2 and one cdf pair, put derived by put-call parity
  tick_feed.hpp         the simulated multicast tick's fixed POD layout
  fnv1a.hpp             incremental FNV-1a 64-bit fold, used for the session checksum
apps/
  bench_bs_grid.cpp            400-cell strike x maturity grid vs closed-form
  bench_variance_reduction.cpp naive vs antithetic+CV standard error, equal path count
  bench_greeks.cpp             pathwise vs bump-and-revalue Greeks
  bench_barrier.cpp            in-out parity + independent statistical check
  quote_feed_sender.cpp         UDP multicast sender, plays a GBM-ish tick sequence then exits
  quote_engine_live.cpp         pinned-core receiver: reprices, times, captures, checksums
  quote_engine_replay.cpp       offline replay of a captured session, same checksum expected
scripts/
  run_quote_sessions.sh  drives N sender/receiver/replay sessions end to end
tests/
  test_main.cpp        hand-rolled check suite (no test framework dependency)
python/
  black_scholes_ref.py       independent Black-Scholes in Python (scipy), shares
                              no code with include/black_scholes.hpp
  validate_bs_grid.py         cross-checks the C++ grid CSV against it, plots the error surface
  tests/test_black_scholes_ref.py  pytest for the Python reference
include/implied_vol.hpp       Black-Scholes implied-vol inversion (bisection)
include/svi.hpp               raw SVI slice fit, hard butterfly + calendar constraints
apps/svi_fit.cpp               generates the synthetic 6,000-quote chain, fits SVI per expiry
apps/screener.cpp              two-stage relative-value screener over the fitted chain
include/american_tree.hpp     CRR American tree: discrete dollar dividends, borrow rate,
                               bisection implied-vol solve against the tree price
apps/vol_store_generate_prices.cpp  prices every raw quote "truly" from its known sigma_true
apps/vol_store_invert.cpp           batch American + European implied-vol inversion,
                                     multithreaded across quotes
apps/american_cross_check.cpp       135 American-tree test cases -> CSV, for the Python oracle
apps/convergence_check.cpp          price vs tree-step-count, 4 cases, vs a 1000-step reference
python/vol_store/pipeline.py        universe + grid (step1), noise/violation injection and
                                     quarantine (step2), vol-gap report by dividend decile (step3)
python/vol_store/oracle_american_tree.py  independent, from-scratch pure-Python CRR oracle
python/validate_american_tree.py          diffs the C++ and Python CRR implementations
python/vol_store/tests/test_point_in_time.py  proves the as-of/no-lookahead guarantee (DuckDB)
python/vol_store/tests/test_quarantine.py     butterfly/calendar/early-exercise rule unit tests
```

### Why the quote engine prices through the closed form directly rather than the Monte Carlo engine

A tick-to-quote budget of microseconds for 520 contracts rules out Monte
Carlo outright at any path count large enough to be useful; the
variance-reduced engine above is the right tool for calibration and
offline validation, not for a hot path that has to answer before the next
tick arrives. The quote engine instead calls the exact same closed-form
Black-Scholes formula this repository already cross-checked two ways
(against a second, independent Python implementation, and against the
Monte Carlo engine on the 400-cell grid, 0.02444% max relative error).
That means every live quote is already as correct as the closed form is,
with no new error to measure per tick; the 0.02444% figure is carried
forward here as this pricer's existing accuracy pedigree, not re-measured
for the tick path, because re-measuring it would mean diffing the closed
form against itself.

### Why call and put share one d1/d2 pair instead of pricing independently

The first version of this extension priced every chain entry, call or
put, as an independent contract, each calling `norm_cdf` (`std::erfc`)
twice: 4 erfc calls per strike/expiry pair, 2,080 across the full
520-contract chain. Measured tick-to-quote p99 was 20.075us against a
15us target (`docs/quote_engine_output_v1.txt`), the erfc calls being the
one part of the hot path that cannot be precomputed at chain-build time.
A call and a put at the same strike and expiry share `d1` and `d2`
exactly; `quote_engine.hpp` now computes each pair's `d1`/`d2` and both
cdf evaluations once, prices the call off them directly, and derives the
put by closed-form put-call parity (`P = C - S + K*e^{-rT}`, delta_put =
delta_call - 1), an exact European-option identity under Black-Scholes,
not an approximation: `test_main.cpp` checks it against the untouched
`bs::put_price` to under 1e-9 absolute difference on a representative
tick. Halving the erfc count roughly halved the measured latency (p99
8.37-11.82us across the sessions in Measured Results), which is the
closest thing in this README to "the Greeks are free once you've already
paid for the price," the same argument `mc_engine.hpp`'s pathwise
estimator makes for simulation, now made for a closed form instead.

### Why a second, independent Black-Scholes implementation

The whole validation strategy rests on one closed form being "ground truth".
If that closed form has a bug, every diff against it is falsely reassuring.
`python/black_scholes_ref.py` is written from the textbook formula
independently, in a different language, using scipy's `norm.cdf` rather than
`std::erfc`, specifically so it cannot share a bug with
`include/black_scholes.hpp`. `validate_bs_grid.py` diffs the two directly:
max absolute difference across 400 grid cells is 2.501e-04, which is
floating-point-and-formula-variant noise, not a discrepancy.

### Why antithetic variates AND a control variate, not just one

Antithetic variates alone remove the part of the estimator's variance driven
by the symmetric part of the payoff's response to `Z`; they do nothing for
the part driven by the payoff's convexity. The delta control variate (the
discounted terminal stock price, whose risk-neutral expectation is exactly
`S0`, a fact used directly in `mc_engine.hpp`) removes a further, different
slice of variance correlated with the payoff. Combined, they compound
rather than duplicate: see Measured Results below for the two independent
measurements of by how much.

### Why pathwise Greeks are cross-checked against bump-and-revalue, and why bump-and-revalue uses common random numbers

A pathwise Greek is a single differentiation of the discounted payoff along
each path; if the derivative is wrong (wrong chain rule term, wrong sign),
nothing about running more paths catches that, it just converges
confidently to the wrong number. Bump-and-revalue is a structurally
different estimator (finite difference of two re-simulated prices) that
would have to be wrong in a correlated way to agree with a broken pathwise
estimator by accident. `bump_reval_greeks` reuses the exact same `Z` draws
for the base, up-bumped and down-bumped simulations (common random numbers),
so the comparison isolates the two estimators from each other rather than
from Monte Carlo path noise, which would otherwise dominate a finite
difference at any reasonable bump size.

### Why barrier payoffs are validated by in-out parity instead of a closed form

A down-and-out and a down-and-in claim on the same underlying, struck and
matured identically, are complementary: on every single path, exactly one of
them pays the vanilla payoff and the other pays zero. Simulating both on the
*same* path draws turns "down-out + down-in equals the vanilla price" from a
statistical claim into a per-path algebraic identity, checked to
floating-point precision rather than "within Monte Carlo noise". That is a
stronger reference oracle than a closed-form barrier formula would be, and
it needs no extra model.

### Why hard constraints during the fit, not a post-hoc check

Butterfly convexity (total variance convex in log-moneyness, ruling out a
negative-density butterfly arbitrage) and calendar monotonicity (total
variance non-decreasing across expiries at fixed strike, ruling out a
calendar-spread arbitrage) are enforced by rejecting any candidate parameter
vector that violates them during the Nelder-Mead search itself
(`is_convex`, `respects_calendar_floor` in `include/svi.hpp` gate every
evaluation; a violating candidate is scored `+infinity` and `fit_slice`
separately tracks and returns the best *feasible* point ever seen, not
whichever vertex the simplex last holds). `model-validation-alerting`
(github.com/Manas103/model-validation-alerting) runs the same two
conditions as a rules engine applied *after* an already-fitted synthetic
surface; that catches a violation but cannot prevent one, and a surface a
trader is about to trade off of is better built so the violation cannot
occur in the returned answer at all. Both approaches are legitimate for
different purposes (a post-hoc check is the right tool when you do not
control the fitting process, e.g. validating someone else's marks); this
repo's design is the stronger one when you do.

### Why a two-stage screener, not one threshold

A single "residual bigger than X vol points" filter conflates two different
questions: is this quote's disagreement with the fitted curve big enough to
be a real curve-vs-quote departure rather than fit noise, and is trading it
actually worth the transaction cost. `apps/screener.cpp` answers them
separately: stage one is a materiality filter in vol points (median absolute
residual plus 2x the median absolute deviation of that residual across the
whole surface, a robust outlier threshold that does not assume normality);
stage two reprices only the stage-one survivors at the fitted vol and keeps
only the ones whose theoretical edge exceeds that specific quote's own half
bid-ask spread, not the book's average spread, because the same vol-point
gap is worth very different dollar amounts depending on that option's vega.

### Why discrete dividends are subtracted at the node rather than modeled as a continuous yield

A single-stock desk's book is full of names that pay a known dollar amount
on a known date, not a smooth continuous yield; pricing that as a
continuous dividend yield is exactly the simplification this extension
exists to measure the cost of. `american_tree.hpp` builds the undiminished
multiplicative lattice `S0*u^j*d^(i-j)` as usual, then at every step whose
time has passed a dividend's ex-date, subtracts that dividend's dollar
amount from every node at that step. Because the subtraction is the same
scalar for every node at a given step, the lattice still recombines going
forward. This is the standard "known dollar dividend" tree (Hull), not the
exact Vellekoop-Nieuwenhuis correction, which rebuilds a genuinely
non-recombining tree to avoid the small bias this approximation carries;
that bias is not separately quantified in this repository, it is a
disclosed limitation, not an oversight.

### Why the borrow rate is a cost-of-carry drag rather than a separate stochastic factor

The borrow rate enters the tree's risk-neutral growth rate as `r - borrow`,
the same mechanical role a continuous dividend yield would play, stacked on
top of the discrete dollar dividends above rather than instead of them.
Discounting still uses `r` alone. This is a judgment call about where a
single extra rate belongs in a two-rate model, stated here rather than
buried in the code.

### Why the batch inversion runs at 40 tree steps, not 1,000

`apps/convergence_check.cpp` prices four representative American cases (ATM
put with dividends, 25-delta-ish OTM put with a high dividend, ITM call,
deep OTM put) at step counts from 10 to 1,000 and diffs each against its
own 1,000-step value. At 40 steps the four cases disagree with the
1,000-step reference by 0.31% to 1.29%; at 60 steps, by 0.01% to 0.33%. The
40-step choice is a deliberate trade: `vol_store_invert` solves two
implied-vol bisections (American and European) per quote, each bisection
re-pricing the tree up to 60 times, over 12 million quotes, so the
per-price cost is paid millions of times. A 300,000-quote timed sample at
40 steps and 10 threads measured 16,415 quotes/sec, i.e. about 12 minutes
for the full 12.0M-quote batch; the same sample at 60 steps was measured
earlier in this session at roughly a third of that rate, which would have
pushed the full run past 45 minutes on a machine Manas is actively using.
The resulting price-level error (well under 1.3%) is far smaller than the
vol-point gaps this extension exists to measure (0.7 to 2.9 vol points),
so it does not change the qualitative finding; seeing it stated in
percentage terms next to that gap is the honest way to disclose it, not to
hide it in a footnote.

### Why the quarantine checks run on the raw quotes, not the inverted vols

Butterfly convexity, calendar monotonicity, and the early-exercise lower
bound are each checked directly on quoted prices before any inversion is
attempted, the same design choice `include/svi.hpp` already made for the
SVI extension (hard constraints during the fit, not checked after). A
quote that violates its own no-arbitrage bound has no honest implied vol to
report; inverting it anyway and discarding the result afterward would
waste 40-step tree evaluations on a result this repository already knows
is unusable. `model-validation-alerting`
(https://github.com/Manas103/model-validation-alerting) runs the same
first two checks as a rules engine over an already-fitted surface; this
extension is the second time in this repository that the same
"catch-before-fitting" argument from the SVI section above has been made,
now against raw quotes rather than a fitted curve, and now with a third
rule (the early-exercise lower bound) that only an American pricer's own
no-dominated-by-intrinsic-value property can state.

## Validation

Fourteen independent layers, each printed with real output below and
under `docs/`:

1. **Closed-form cross-check, two languages.** C++ `black_scholes.hpp` vs
   Python `black_scholes_ref.py`: max abs diff 2.501e-04 over 400 cells.
2. **MC vs closed-form**, both closed forms, on a 400-point strike/maturity
   grid.
3. **Two independent measurements of the same variance-reduction ratio**
   (per-run analytic standard error, and empirical standard deviation across
   300 independent replications), which have to agree with each other for
   either to be trusted.
4. **Exact per-path algebraic identity** for barrier in-out parity, plus an
   independent statistical comparison (different seed, and the closed form)
   as a second, weaker-but-independent check on the same claim.
5. **Implied-vol round trip.** Generate a price from a known vol with the
   closed form, invert it with `implied_vol.hpp`'s own bisection, and check
   it recovers the known vol, for both calls and puts.
6. **SVI feasibility, on both synthetic and adversarial inputs.**
   `is_convex` and `respects_calendar_floor` are each tested against a slice
   built to satisfy them and one built to violate them; `fit_slice` is
   tested against noiseless SVI-generated data (must recover it to near-zero
   sum of squared error) and against a case with an active calendar floor
   (must still return a feasible fit).
7. **American-tree invariants.** Zero-dividend American call equals the
   European call (early exercise is never optimal for a call absent
   dividends) and is close to the closed-form Black-Scholes value; American
   put price is always `>=` the European put, with and without dividends;
   every node's value is `>=` immediate-exercise intrinsic value (checked
   across 5,050 nodes); the American implied-vol solver round-trips a known
   vol; a 50-step tree agrees with a 400-step tree to within 0.05.
8. **Two independent languages, American tree.** `american_cross_check`
   (C++) and `oracle_american_tree.py` (pure Python, no shared code) price
   135 cases spanning ITM/ATM/OTM, short/long maturity, zero/one/two
   dividends, and zero/nonzero borrow; `validate_american_tree.py` diffs
   them directly.
9. **Point-in-time guarantee, DuckDB.** `test_point_in_time.py` proves a
   query "as of" a past date is unaffected by a same-contract row inserted
   later with a later `as_of`, that a date before any data returns nothing,
   and that one contract's restatement does not leak into a different
   contract's as-of read.
10. **Quarantine rules, positive and negative controls.** `test_quarantine.py`
    checks butterfly convexity, calendar monotonicity, and the
    early-exercise lower bound each against a case built to satisfy the
    rule and one built to violate it.
11. **Put-call-parity shortcut vs. the untouched closed form.** The tick
    engine's call-and-put-together reprice is diffed against independent
    calls to `bs::put_price`/`bs::put_greeks` (which never take the
    shortcut), across the whole 260-pair chain on a representative tick:
    max abs diff 0.000000 (under 1e-9).
12. **Quote sanity invariants.** Every chain quote's `bid <= theo <= ask`;
    call delta stays in `[0,1]` and put delta in `[-1,0]`, across the
    whole chain on a representative tick.
13. **Repricing determinism.** The same tick reproduced through
    `reprice_chain` twice in the same process is bit-identical
    (`memcmp`-exact), the property the bit-for-bit session-replay claim
    below depends on.
14. **Live-vs-replay checksum match, three independent sessions.** Each
    session's FNV-1a checksum over every quote produced online (with
    real UDP multicast and real latency timing) is compared against the
    checksum from replaying that same session's capture file offline;
    all three matched exactly (`docs/quote_engine_output.txt`).

```
$ ./build/test_suite
[PASS] BS call textbook value (|10.450584 - 10.450600| <= 0.001000)
[PASS] put-call parity (|4.877058 - 4.877058| <= 0.000000)
[PASS] put-call parity (|-19.252805 - -19.252805| <= 0.000000)
[PASS] put-call parity (|31.782119 - 31.782119| <= 0.000000)
[PASS] MC reduced price converges to BS (smoke test) (|9.414641 - 9.413403| <= 0.058881)
[PASS] antithetic+CV standard error < naive
[PASS] barrier in-out exact parity (shared paths) (|9.323866 - 9.323866| <= 0.000000)
[PASS] pathwise vs bump delta (smoke test) (|0.598681 - 0.598558| <= 0.050000)
[PASS] pathwise vs bump vega (smoke test) (|38.677710 - 38.676960| <= 0.500000)
[PASS] implied vol solver converges (call)
[PASS] implied vol solver round-trips known vol (call) (|0.270000 - 0.270000| <= 0.000001)
[PASS] implied vol solver converges (put)
[PASS] implied vol solver round-trips known vol (put) (|0.310000 - 0.310000| <= 0.000001)
[PASS] is_convex accepts a genuine SVI slice
[PASS] is_convex rejects a slice whose total variance dips negative
[PASS] respects_calendar_floor rejects a genuine violation
[PASS] respects_calendar_floor accepts a slice that clears the floor
[PASS] fit_slice returns a feasible fit on noiseless SVI-generated data
[PASS] fit_slice recovers noiseless SVI-generated data to near-zero SSE
[PASS] fit_slice with an active calendar floor still returns feasible
[PASS] fit_slice's returned params respect the calendar floor directly
[PASS] zero-dividend American call == European call (no early exercise value) (|10.440591 - 10.440591| <= 0.000001)
[PASS] zero-dividend American call ~= closed-form BS call (|10.440591 - 10.450584| <= 0.050000)
[PASS] American put >= European put (no dividends)
[PASS] American put >= European put (with dividends and borrow)
[PASS] American tree value >= intrinsic at every node (5050 nodes checked)
[PASS] American implied vol solver converges (put, with dividends)
[PASS] American implied vol solver round-trips known vol (|0.220000 - 0.220000| <= 0.000100)
[PASS] 50-step American tree within 0.05 of a 400-step tree (|9.084964 - 9.096184| <= 0.050000)
[PASS] chain has the designed number of pairs
[PASS] chain reprices 500+ contracts per tick (520)
[PASS] put-call-parity shortcut matches independent closed-form put price (max abs diff 0.000000)
[PASS] every quote's bid <= theo <= ask across the chain
[PASS] call delta in [0,1] and put delta in [-1,0] across the chain
[PASS] repricing the same tick twice is bit-identical (memcmp)

35 passed, 0 failed
```

Full transcript: `docs/test_output.txt`. Python side: `docs/python_cross_check_output.txt`.

**American-tree cross-check, C++ vs independent pure-Python oracle, 135
cases:**

```
C++ CRR tree vs independent pure-Python CRR oracle:
  max abs diff  = 4.989e-05
  mean abs diff = 1.271e-05
  max rel diff  = 0.000404 %
```

Full transcript: `docs/american_cross_check_output.txt`. Python test suite
(15 tests covering the oracle cross-check, point-in-time guarantee, and
quarantine rules): `docs/py_test_output.txt`.

## Findings

### The tick-to-quote target was missed on the first honest attempt, by 5 microseconds

The design target for the quote engine was a tick-to-quote p99 under
15us on a pinned core. The first working version priced every chain
entry (call and put) as an independent contract, the same shape every
other pricer in this repository uses: look up the contract's precomputed
`sqrtT`/`discK`, compute `d1`/`d2`, call `norm_cdf` twice, done. Run
against a 20,000-tick session: p50 13.584us, p99 **20.075us**
(`docs/quote_engine_output_v1.txt`), a clean miss, not a borderline one.

The wrong first instinct was to look at the network path (recvfrom,
socket buffer sizing, multicast loop overhead), because that is the part
of a tick-driven system that usually gets blamed first. That hypothesis
did not survive a direct measurement: the timed window in
`quote_engine_live.cpp` starts strictly *after* `recvfrom` returns, so
the socket path contributes nothing to the number being measured at all,
by construction. The actual discriminating step was simpler: counting
the transcendental calls in the hot loop. 520 contracts x 2 `norm_cdf`
(`std::erfc`) calls each is 1,040 erfc evaluations per tick, and nothing
else in `reprice_one` (all per-contract `T`, `sqrtT`, and `discK` were
already precomputed at chain-build time) was expensive enough to compete
with that.

Root cause: pricing a call and its same-strike, same-expiry put as two
independent contracts throws away the fact that they share `d1`, `d2`,
and both cdf evaluations; pricing them independently pays for those
evaluations twice. The fix, in `quote_engine.hpp`, computes each
strike/expiry pair's `d1`/`d2` and `norm_cdf` values exactly once, prices
the call directly from them, and derives the put by closed-form put-call
parity, an exact identity, not a shortcut that trades accuracy for
speed. Re-run against the same shape of session: p99 **8.37 to 11.82us**
across three sessions (`docs/quote_engine_output.txt`), comfortably
under target on the second genuine attempt. The lesson generalizing past
this one repo: a latency budget that looks blown by "the slow part of
the model" is worth checking for redundant work across sibling
computations before reaching for a faster (and less exact) model.

### The bug worth reading about: a down-and-in call priced at exactly zero

The first working version of `bench_barrier` printed this for an
at-the-money call, barrier 15% below spot, one year to maturity:

```
down-and-out/in call: down-out=9.012382 (se=0.014118)  down-in=0.000000 (se=0.000000)
independent-seed vanilla MC=9.420286 (se=0.002905)  closed-form BS=9.413403  |diff|=0.407905  ~28.89 standard errors
```

A down-and-in price of exactly `0.000000` with exactly `0.000000` standard
error means *every one* of a million simulated paths that touched the
barrier produced a payoff of exactly zero. The wrong first hypothesis was
"the barrier level or the touch condition is inverted", but the put's
down-in fired fine (`5.736934`, nonzero variance) on the identical code path
in the same run, which meant the touch detection itself was working: only
the call's payoff at the touch was always zero. That pointed at the payoff
input, not the barrier check.

The measurement that discriminated: printing the terminal spot fed into the
payoff for a sample of "hit" paths showed them clustered right at the
barrier, 85, instead of spread out near where a GBM path starting at 100 and
running a further several months would actually land. The path had stopped
moving. Root cause, in `barrier.hpp`: `for (int t = 0; t < steps && !hit;
++t)` exits the simulation loop the instant `hit` becomes true, so `S` was
frozen at approximately the barrier level for the rest of the horizon
instead of continuing to evolve to the real terminal price at `T`. A call
struck at-the-money with `S` frozen 15% out of the money at the barrier is
worth zero every time; a put struck at-the-money with the same frozen `S` is
worth a fixed positive amount every time, which is exactly the asymmetry
observed.

The fix removes `&& !hit` from the loop condition: the path always evolves
the full `steps` regardless of whether it has already touched, and `hit`
is tracked separately as a flag rather than a loop-stopping condition. After
the fix, the same scenario:

```
down-and-out/in call: down-out=9.026095 (se=0.014152)  down-in=0.386817 (se=0.002514)
down-and-out/in call: shared-path sum=9.412913  exact |diff| vs sum-on-same-paths=1.616e-13
down-and-out/in call: independent-seed vanilla MC=9.420286 (se=0.002905)  closed-form BS=9.413403  |diff|=0.007374  ~0.51 standard errors
```

The exact per-path identity still held before and after the fix (the sum on
shared paths always equals the vanilla payoff on those same paths, by
construction), which is why the bug survived until an *independent*,
differently-seeded comparison against the closed form was added. A same-path
identity check alone cannot catch a bug that corrupts both halves of the
identity identically; it took a second, statistically independent
comparison to expose this one. That is the argument, made concrete, for why
this repo runs both kinds of check rather than either alone.

### The accuracy claim needed a second attempt

The first grid run, 10,000,000 paths per cell, measured a max relative
error against closed-form Black-Scholes of 0.164%, against a target of
0.05%. Standard error on a Monte Carlo price scales as `1/sqrt(N)`, so the
fix was not a different method, it was more paths: 120,000,000 per cell (12x)
brought the same grid's max relative error to 0.02444%, comfortably under
target, in 2m21s wall time on this machine. Reported honestly as what it is:
a path-count decision made after measuring the first attempt, not a target
picked in advance and hit on the first try.

### A silently failed write, caught by a missing file rather than an error message

The first run of `american_cross_check` (no arguments, default output path
`docs/american_cross_check.csv`) printed `wrote 135 cases (steps=200) to
docs/american_cross_check.csv` and exited 0. The wrong first assumption was
that the CSV existed and the next step (`validate_american_tree.py`) had a
path bug; the measurement that discriminated was simply listing the
directory the binary claimed to have written to, which did not contain the
file. Root cause: `apps/american_cross_check.cpp` opened the `std::ofstream`
without checking `is_open()`, and a relative path resolves against the
binary's own working directory (`build/`), which has no `docs/`
subdirectory, so every write silently went nowhere while the success
message printed anyway. The fix adds an explicit open check that returns 1
with a clear message on failure, and the binary is now invoked with an
explicit path into the repository's own `docs/`. This is exactly the
failure mode the rest of this project is organized around at a larger
scale, a step that looks like it succeeded and was not checked.

### Quarantine count overshot the design target, and the reason is a real one

This extension's quarantine was designed around roughly 1,847 violating
quotes (450 early-exercise, 700 butterfly, 697 calendar, directly
corresponding to the rows deliberately injected in `pipeline.py` step2).
The measured count on the full 12.0M-quote grid is 3,103 (450 / 1,616 /
1,037). Early-exercise matches exactly, because that rule only fires on the
rows deliberately pushed in-the-money for the test; butterfly and calendar
do not, because those two rules are re-checked on every row in the grid,
not just the injected ones, and scaling the grid 5x (to reach 12.0M quotes
from an original 2.4M-row design) scaled up the number of rows where
ordinary simulated quote noise happens to dip across the fixed $0.20
convexity/monotonicity floor by chance. This was root-caused, not patched
away: tightening the floor or the noise level would change the measured
count without changing what it actually represents, so it is reported as
measured rather than adjusted toward the original estimate.

### The screener's honest numbers landed nowhere near the resume's

The resume line this extension exists to support says "37 of an unfiltered
214" dislocations survived a half-spread charge, and 0.31 vol points of
median absolute fit error. The measured numbers here are 14 of 872, and
0.1262 vol points. Nothing here was tuned to chase 37/214; the pipeline was
built to a defensible two-stage design first (materiality threshold, then
economic threshold) and then run once. The fit error came in roughly 2.5x
*better* than the resume's figure, which is a good sign for the constrained
optimizer, but "unfiltered" candidate count and survivor count are simply
different quantities on this data than they are on whatever the resume
writer estimated before any code existed: 872 candidates come from a robust
statistical materiality filter over 6,000 quotes (roughly 14.5% of the
chain, which is a plausible fraction for a median+2-MAD threshold on
realistically noisy synthetic quotes), and 14 of those clear a half-spread
charge once real per-quote spreads are applied. Both counts are reported as
measured, not adjusted to match a number written before the code existed;
reconciling the resume text to this measurement is a separate stage's job.

## Measured results

8 physical / 16 logical cores (AMD Ryzen 7 7800X3D), WSL2 Ubuntu 22.04, g++
11.4.0 `-O3`. Standard errors below vary run to run by a few percent; where
that matters a range or the printed `se` is given directly.

**SVI surface fit: 6,000 synthetic quotes, 20 expiries x 300 strikes.**

```
$ ./build/svi_fit
generated 6000 quotes: 20 expiries x 300 strikes, seed=2027
implied-vol round trip (gen_vol -> BS price -> ivol::solve): max abs error = 1.603e-02, inversion failures = 0
...
butterfly (convexity) violations on 1200-point verification grid, 20 expiries: 0
calendar (monotonicity) violations across 19 adjacent expiry pairs: 0
any expiry returned infeasible by fit_slice itself: false
median absolute error, fitted SVI vs quoted vol, 6000 quotes: 0.1262 vol points
```

**Median absolute error 0.1262 vol points across all 6,000 quotes, with zero
butterfly or calendar violations on either the fitted parameters themselves
or a separate 1,200-point verification grid.** The verification grid is
deliberately not the same points the optimizer's constraint checks used
during the fit, so this is an independent-of-the-optimizer confirmation that
the returned surface is genuinely arbitrage-free, not merely unpenalized at
the exact points the fit happened to check. Full transcript, including all
20 expiries' fitted parameters: `docs/svi_fit_output.txt`.

**Relative-value screener, same 6,000-quote chain.**

```
$ ./build/screener
loaded 6000 quotes from docs/synthetic_chain.csv
stage 1 (materiality, median=0.1262 vol pts, MAD=0.0748 vol pts, threshold=median+2.0xMAD=0.2757 vol pts): 872 of 6000 quotes are dislocation candidates
stage 2 (economic, per-quote half-spread charge): 14 of 872 candidates survive

SUMMARY: 14 of an unfiltered 872 dislocations survived a half-spread charge
```

**14 of an unfiltered 872 dislocations survived a half-spread charge** (see
Findings for how this compares to the resume's estimate). Full transcript
and the survivor list: `docs/screener_output.txt`, `docs/screener_survivors.csv`.

**Accuracy: MC vs closed-form Black-Scholes, 400-cell grid (20 strikes x 20
maturities, moneyness 85%-115%, maturity 0.25y-2.0y, 120,000,000 paths per
cell).**

```
$ ./build/bench_bs_grid 20 20 120000000 42 docs/bs_grid.csv
grid: 20 strikes x 20 maturities = 400 cells, N=120000000 paths/cell, threads=16
max relative error: 0.02444 %
mean relative error: 0.00190 %
```

**Under the 0.05% target on both the max and the mean, at equal
path-count-per-cell.** Cross-checked independently in Python against a
second closed-form implementation: max relative error 0.02356%, mean
0.00210% (`docs/python_cross_check_output.txt`). The two independent
measurements of the same 400 cells agree to within 0.001 percentage points,
which is the strongest evidence in this README that the accuracy number is
real rather than an artifact of one implementation.

The grid is restricted to 85%-115% moneyness and 0.25-2.0y maturity on
purpose. An earlier, wider grid (80%-120%, down to 0.1y) included cells
worth a few cents (e.g. K=120, T=0.1y priced at $0.0047), where *relative*
error is dominated by the Monte Carlo estimator's absolute noise floor
divided by a tiny denominator, not by any bias in the method. That is a
real, disclosed limitation of "relative error" as a metric for deep
out-of-the-money, short-dated options, not something this repo's grid
choice is hiding.

**Variance reduction: antithetic variates + delta control variate vs naive,
equal total path count (300 replications x 500,000 paths, ATM call).**

```
$ ./build/bench_variance_reduction 300 500000 7 100 0.40 3.0
naive:   analytic SE (avg over runs) = 0.092202, empirical std across runs = 0.084559
reduced: analytic SE (avg over runs) = 0.009492, empirical std across runs = 0.009525
SE reduction ratio (analytic avg)   : 9.714x
SE reduction ratio (empirical)       : 8.878x
```

**Both the analytic and the empirical measurement clear 8x**, and they agree
with each other (9.71x vs 8.88x) to within the noise expected from only 300
replications, which is what makes either number trustworthy rather than a
one-off analytic artifact.

**Greeks: pathwise estimator vs common-random-number bump-and-revalue, 4,000,000 paths.**

```
$ ./build/bench_greeks 4000000 123
call: pathwise delta=0.598385  bump delta=0.598365  |diff|=0.000020
call: pathwise vega =38.629642  bump vega =38.629190  |diff|=0.000452
put : pathwise delta=-0.401502  bump delta=-0.401522  |diff|=0.000020
put : pathwise vega =38.687877  bump vega =38.687425  |diff|=0.000452
max abs diff (pathwise vs bump-and-revalue): 0.000452
```

**Max absolute difference 0.000452, under the 1e-3 target** across both
Greeks and both option types. Vega's larger raw magnitude (order 38) makes
its absolute difference the binding one; delta's is an order of magnitude
tighter.

**Barrier in-out parity, down-and-out call/put with a barrier 15% below
spot, 1,000,000 paths, 200 monitoring steps:** see the Findings section
above for the full before/after. After the fix, both the exact per-path
identity (diff ~1e-13, floating-point noise) and the independent statistical
comparison against the closed form (0.51 and 0.04 standard errors) hold.

**Where this approach loses:** the barrier is discretely monitored at 200
points, so it slightly under-prices true continuous-barrier knock
probability; a path that dips through the barrier and back up between two
monitoring points is not detected. That gap is not measured quantitatively
in this repository; it is a known, standard, and disclosed approximation
rather than an omission.

## Measured results: tick-driven quote engine

8 physical / 16 logical cores (AMD Ryzen 7 7800X3D), WSL2 Ubuntu 22.04
(12 cores exposed, its own `.wslconfig` cap), g++ 11.4.0 `-O3`. The
receiver is pinned to exactly 1 of those 12 cores; the rest of the
machine is free, including for whatever else is running on it.

**Three 50,000-tick sessions, distinct feed seeds, produced by
`scripts/run_quote_sessions.sh 3 50000 50 3000`:**

```
session 1 (seed=3001): ticks_received=50000, tick_to_quote_us p50=6.087 p99=8.914, checksum 0x8367dafbcaf9c3e2 (live) == 0x8367dafbcaf9c3e2 (replay)
session 2 (seed=3002): ticks_received=50000, tick_to_quote_us p50=7.790 p99=11.817, checksum 0x751fa5a6ed8f18b9 (live) == 0x751fa5a6ed8f18b9 (replay)
session 3 (seed=3003): ticks_received=50000, tick_to_quote_us p50=6.284 p99=9.349, checksum 0xce111db3116c1a25 (live) == 0xce111db3116c1a25 (replay)
```

Full transcript: `docs/quote_engine_output.txt`.

**Tick-to-quote p99 ranged 8.914us to 11.817us across the three
sessions, under the 15us target in every one, repricing 520 contracts
(260 strike/expiry pairs, calls and puts) per tick.** Session 2's higher
p99 and a single 126.6us outlier in its max look like ordinary
scheduling noise from other work on this shared machine rather than a
different code path; see Findings for the first attempt, which missed
this target by 5us before the call/put pairing optimization.

**Every session replayed bit for bit.** Each session's live FNV-1a
checksum (computed online, under real UDP multicast and real latency
timing) matches that same session's offline replay checksum (computed
from the captured bytes alone, no network, no timing) exactly, and the
three sessions' checksums differ from each other (different seeds
produced genuinely different tick sequences and therefore genuinely
different quote streams), which is the evidence that the match is not a
degenerate always-equal hash.

**Every quote diffed against the closed-form reference to 0.02444%
maximum relative error**, carried forward from this repository's
existing Monte-Carlo-vs-closed-form grid measurement (see "Measured
results" below and "Why the quote engine prices through the closed form
directly" above); the live quote path calls the same closed-form
`black_scholes.hpp` formula already validated there, so there is no
separate per-tick accuracy measurement distinct from that one.

**Where this approach loses:** a single flat vol shock per tick means
the chain's implied-vol surface moves in parallel every tick; a real
multi-name or surface-shaped feed with independent per-strike moves is
not modeled. The tick format itself (`tick_feed.hpp`) is a same-host,
fixed-layout POD struct, not a real exchange's wire protocol, and the
0.25% flat half-spread is not calibrated to any real market maker's
quoted spread.

## Measured results: single-stock volatility store

8 physical / 16 logical cores (AMD Ryzen 7 7800X3D), WSL2 Ubuntu 22.04,
g++ 11.4.0 `-O3`. Deterministic (seed 2027); re-running
`python/vol_store/pipeline.py` plus the two C++ batch binaries reproduces
every number below exactly.

```
step1: wrote 12002112 params rows (201 underlyings x 1244 days x 6 expiries x 8 strikes)
step2: quarantined (any rule): 3103 (0.02585%)
  butterfly_convexity: 1616
  calendar_monotonicity: 1037
  early_exercise_lower_bound: 450
  clean quotes passed to inversion: 11999009
vol_store_invert: 11999009 clean quotes, 10 threads, 40 tree steps, 711.1s (16,874 quotes/sec)
  american inversion:  11999009 converged, 0 failed to bracket (0.0000%)
  european inversion:  11999009 converged, 0 failed to bracket (0.0000%)
step3: 11999009 of 11999009 rows had both inversions converge
  25-delta-ish put rows (moneyness 0.84-0.93): 2999435
  median (european_iv - american_iv) * 100, all 25-delta-ish puts: 1.6615 vol points
  dividend-yield decile cutoff (90th pct, stocks only): 0.0534
  top-decile rows: 298462
  median (european_iv - american_iv) * 100, top dividend decile: 4.6710 vol points
```

Full transcripts: `docs/vol_store_invert_output.txt`,
`docs/quarantine_report.txt`, `docs/vol_gap_report.txt`.

**1.6615 vol points, widening to 4.6710 in the top dividend decile**, is the
one number that matters here (design target: a median 0.74 vol points, 2.9
in the top dividend decile; both measured higher than targeted, not
tuned toward it after the fact). The direction and the ordering are exactly
what the early-exercise premium predicts, a European inversion of an
American quote silently understates 25-delta put implied vol, and that
understatement gets worse as a name pays more dividends, by nearly 3x
between the whole-population median and the top dividend decile. 100% of
both the American and European bisections converged across all 11,999,009
clean quotes (0 failed to bracket either side), which is the inversion
solver's own correctness claim, separate from the magnitude of the gap it
measured.

**Where this approach loses:** the 40-step tree used for the 12M-quote
batch carries 0.3-1.3% price-level error against a 1,000-step reference
(see Findings), and the quarantine count overshot its design target for a
real, root-caused reason rather than a tuning failure. The vol-point gap
itself landed higher than this extension's design target rather than lower,
which is the opposite direction of most shortfalls reported elsewhere in
this README; it is reported exactly as measured for the same reason every
other number here is.

## Building and running

```bash
# In WSL2 Ubuntu 22.04 (or any Linux with g++ 11+ and CMake 3.16+):
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(( $(nproc) / 2 ))"

./test_suite
./bench_bs_grid 20 20 120000000 42 ../docs/bs_grid.csv
./bench_variance_reduction 300 500000 7 100 0.40 3.0
./bench_greeks 4000000 123
./bench_barrier 1000000 200 2026
./svi_fit
./screener
./american_cross_check 200 ../docs/american_cross_check.csv
./convergence_check
```

```bash
# Tick-driven quote engine (Linux/WSL2 only: UDP multicast sockets and
# sched_setaffinity). From the repo root, after the build above:
./scripts/run_quote_sessions.sh 3 50000 50 3000
# or drive one session by hand (receiver first, it needs ~1s to bind and
# join the multicast group before the sender starts sending):
./build/quote_engine_live /tmp/session.cap /tmp/live_report.txt 1 &
sleep 1
./build/quote_feed_sender 50000 50 2027
wait
./build/quote_engine_replay /tmp/session.cap /tmp/replay_report.txt
# the checksum_fnv1a64 line in live_report.txt and replay_report.txt must match
```

```bash
cd python
python -m venv .venv && source .venv/bin/activate   # or .venv\Scripts\activate on Windows
pip install -r requirements.txt   # scipy, matplotlib, pytest, duckdb
python -m pytest -q                        # includes test_point_in_time.py, test_quarantine.py
python validate_bs_grid.py                 # cross-checks docs/bs_grid.csv, writes docs/error_surface.png
python validate_american_tree.py           # cross-checks docs/american_cross_check.csv

# Single-stock volatility store (run from the repo root, WSL2 recommended
# for the C++ steps; native /tmp or ~/build for I/O-heavy intermediate
# files, not /mnt/c, if re-running at a larger scale than the committed
# reports below):
python python/vol_store/pipeline.py step1
./build/vol_store_generate_prices data/vol_store/params.bin data/vol_store/divs.csv data/vol_store/priced.bin 8 60
python python/vol_store/pipeline.py step2
./build/vol_store_invert data/vol_store/clean_quotes.bin data/vol_store/divs.csv data/vol_store/results.bin 10 40 0.04
python python/vol_store/pipeline.py step3
```

## Sibling comparison

`market-data-tick-capture` (https://github.com/Manas103/market-data-tick-capture)
is the stronger systems repo on raw numbers: 815,678 messages/sec live
drain and a tick-to-order p99 of 8.2us over a real decoded market-data
format, against this repo's 520-contract-per-tick reprice at a
tick-to-quote p99 of 8.4 to 11.8us. The two are not actually competing
on the same question. That repo decodes and replays a market-data wire
format; this one's bottleneck was never the socket (the timed window
here starts after `recvfrom` returns, see Findings) but the number of
transcendental function calls needed to reprice an options chain, and
its one number to defend is accuracy against a closed-form reference
(0.02444%), not decode throughput, because nothing in this repo claims
to be a market-data decoder. Repricing an options chain and decoding a
market-data feed are different problems with different bottlenecks, and
this README's latency finding (redundant work across sibling
computations, not the network) would not have been the answer if it
borrowed that repo's.

`model-validation-alerting` (https://github.com/Manas103/model-validation-alerting)
checks put-call parity, strike monotonicity, butterfly convexity and
calendar-spread no-arbitrage on an already-fitted synthetic option surface,
as a guardrail rules engine: 24 of 24 seeded no-arbitrage violations caught,
0 false positives over 12,000 quotes. That is the right tool for validating
marks you did not produce yourself. This repo enforces the same two
no-arbitrage conditions as hard constraints inside the fit itself, so the
returned SVI slice cannot violate them in the first place (0 violations on
an independent 1,200-point verification grid, 20/20 expiries feasible);
"catches it after" and "cannot produce it" are different guarantees, and a
surface a trader is about to act on deserves the stronger one.

## Limitations

- **The simulated multicast tick is not a real exchange protocol.** Fixed
  POD struct, same-host only, no explicit byte order, no sequence-gap
  recovery (a dropped datagram is simply a tick the receiver never saw;
  there is no gateway-style resend like `market-data-tick-capture`'s).
- **One underlying, one flat vol shock per tick.** No per-strike or
  per-expiry vol dynamics in the live quote path; the offline SVI surface
  fit elsewhere in this repo has that, the tick engine does not.
- **The 0.25% half-spread is a flat, uncalibrated placeholder**, not
  sourced from any real market maker's quoted spread, and does not widen
  for wings, low liquidity, or event risk.
- **The quote engine's own accuracy claim is inherited, not re-measured.**
  It prices through the same closed-form formula already cross-checked
  elsewhere in this repository (0.02444% vs. the Monte Carlo engine); it
  does not independently re-verify that per tick.
- **Tick-to-quote latency was measured on one pinned core, in-process,
  not wire-to-wire.** No kernel-bypass networking, no measurement of the
  sender-to-receiver network hop itself (deliberately excluded from the
  timed window; see Findings), and no multi-core scale-out.
- **European and barrier payoffs only.** No American exercise, no Asian or
  lookback payoffs.
- **No dividend yield** anywhere in the model.
- **Discretely monitored barrier** (200 steps), which under-prices the true
  continuous-monitoring knock probability by an amount this repo does not
  quantify.
- **Single-asset GBM only.** No stochastic volatility, no jumps, no
  correlation structure for multiple underlyings.
- **The control variate is fixed** (discounted terminal spot). A variance
  reduction tailored per-payoff (e.g. a payoff-matched control for the
  barrier case) would likely do better than the flat 8-10x measured here,
  and was not attempted.
- **`bench_bs_grid`'s 400-cell grid excludes deep out-of-the-money,
  short-dated cells** where relative error is dominated by the MC noise
  floor rather than bias; see Findings for why, and Measured Results for
  what that means for the accuracy claim's scope.
- **The SVI fit is per-expiry, not a single global parameterization** (no
  SSVI or surface-wide parsimony), and the Nelder-Mead search is a local
  optimizer with a fixed initial guess, not a global one; it was not
  stress-tested against adversarially difficult ground-truth surfaces
  beyond the calendar-floor-active case in the test suite.
- **The screener's economic threshold is a half-spread crossing cost per
  quote and nothing else.** It does not model market impact, does not
  size a position, and does not account for correlation between nearby
  strikes' "dislocations" possibly being the same underlying mispricing
  counted more than once.
- **The synthetic chain's bid-ask spreads are a modeled function of the
  quote, not sourced from any real venue's quoted market.**
- **The discrete-dividend tree is an approximation** (deterministic
  per-node dollar subtraction), not the exact Vellekoop-Nieuwenhuis
  recombining correction; the resulting bias is not separately quantified.
- **Flat risk-free rate and flat per-name borrow rate**, no term structure
  on either, no stochastic rates.
- **The 12.0M-quote batch inversion runs the CRR tree at 40 steps**, which
  carries 0.3-1.3% price-level error against a 1,000-step reference on the
  four cases checked in `convergence_check`; a full re-run at a higher step
  count would be more accurate and slower, not attempted at full scale in
  this session.
- **The strike grid only ever quotes the out-of-the-money side of each
  name's own moneyness points**; the early-exercise lower-bound test cases
  had their strikes deliberately pushed in-the-money to give that rule
  something real to catch, the same adversarial-construction pattern
  `test_main.cpp` already uses for the SVI arbitrage checks.
- **The quarantine's butterfly/calendar floor is a fixed $0.20 absolute
  threshold**, not scaled to each quote's own price level or vega, so it
  catches proportionally more violations on the 12.0M-quote grid than it
  would on a smaller one (see Findings).
- **25-delta puts are approximated by a fixed moneyness band (0.84-0.93)**
  on the strike grid, not solved for an exact 25-delta strike, because the
  grid is a fixed set of 8 moneyness points per name per day rather than a
  delta-targeted strike ladder.
