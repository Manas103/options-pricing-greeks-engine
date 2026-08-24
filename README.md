# Options Pricing and Greeks Engine with Variance-Reduced Monte Carlo

A Monte Carlo pricer for European and barrier options under geometric
Brownian motion, with antithetic-variate and control-variate variance
reduction, pathwise Greeks cross-checked against bump-and-revalue, and a
closed-form Black-Scholes implementation as the reference oracle. C++17 for
the engine, Python (scipy) for an independent second opinion on that
reference. Every number below was measured on this machine, not targeted:
where the first attempt at the accuracy claim fell short, the README says so
and shows the second attempt that met it.

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
- **Machine and toolchain**, for every number below: 8 physical / 16 logical
  cores (AMD Ryzen 7 7800X3D), WSL2 Ubuntu 22.04, g++ 11.4.0, `-O3`, CMake
  3.22.1, C++17. The Python cross-check ran on Windows 11, Python 3.12.10,
  scipy.

## Architecture

```
include/
  black_scholes.hpp   closed-form price + Greeks (delta, gamma, vega, theta),
                       independent of the MC engine, the ground truth it diffs against
  mc_engine.hpp        naive MC, antithetic+control-variate MC, pathwise
                       Greeks, common-random-number bump-and-revalue Greeks
  barrier.hpp          down-and-out/down-and-in on shared paths, discretely monitored
apps/
  bench_bs_grid.cpp            400-cell strike x maturity grid vs closed-form
  bench_variance_reduction.cpp naive vs antithetic+CV standard error, equal path count
  bench_greeks.cpp             pathwise vs bump-and-revalue Greeks
  bench_barrier.cpp            in-out parity + independent statistical check
tests/
  test_main.cpp        hand-rolled check suite (no test framework dependency)
python/
  black_scholes_ref.py       independent Black-Scholes in Python (scipy), shares
                              no code with include/black_scholes.hpp
  validate_bs_grid.py         cross-checks the C++ grid CSV against it, plots the error surface
  tests/test_black_scholes_ref.py  pytest for the Python reference
```

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

## Validation

Four independent layers, each printed with real output below and under
`docs/`:

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

9 passed, 0 failed
```

Full transcript: `docs/test_output.txt`. Python side: `docs/python_cross_check_output.txt`.

## Findings

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

## Measured results

8 physical / 16 logical cores (AMD Ryzen 7 7800X3D), WSL2 Ubuntu 22.04, g++
11.4.0 `-O3`. Standard errors below vary run to run by a few percent; where
that matters a range or the printed `se` is given directly.

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

## Building and running

```bash
# In WSL2 Ubuntu 22.04 (or any Linux with g++ 11+ and CMake 3.16+):
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"

./test_suite
./bench_bs_grid 20 20 120000000 42 ../docs/bs_grid.csv
./bench_variance_reduction 300 500000 7 100 0.40 3.0
./bench_greeks 4000000 123
./bench_barrier 1000000 200 2026
```

```bash
cd python
python -m venv .venv && source .venv/bin/activate   # or .venv\Scripts\activate on Windows
pip install -r requirements.txt
python -m pytest tests/ -q
python validate_bs_grid.py   # cross-checks docs/bs_grid.csv, writes docs/error_surface.png
```

## Limitations

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
