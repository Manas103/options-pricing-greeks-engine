"""Single-stock volatility store pipeline: builds a synthetic 200-single-
name + 1-index universe with discrete dollar dividends and borrow rates,
lays out a strike/expiry EOD quote grid, calls the C++ CRR engine to price
every quote "truly" (American, dividends, borrow), injects a controlled,
disclosed number of no-arbitrage violations, quarantines them, and exports
the clean quotes for the C++ batch inversion (vol_store_invert).

Run as a sequence of steps (a thin C++ call sits between step1 and step2,
and between step2 and step3; see docs/vol_store_benchmark_output.txt for the
exact commands used):

    python3 pipeline.py step1   # universe + grid -> params.bin, divs.csv, universe.csv
    <run vol_store_generate_prices on params.bin -> priced.bin>
    python3 pipeline.py step2   # noise, violation injection, quarantine -> clean_quotes.bin
    <run vol_store_invert on clean_quotes.bin -> results.bin>
    python3 pipeline.py step3   # join results with universe, compute the vol-gap claims

All random draws are seeded (2027) for reproducibility. Everything here is
synthetic; see README "Honest framing, up front".
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np

SEED = 2027
DATA = Path(__file__).resolve().parent.parent.parent / "data" / "vol_store"
DATA.mkdir(parents=True, exist_ok=True)

N_STOCKS = 200
N_UNDERLYINGS = N_STOCKS + 1   # + 1 index, underlying_id == N_STOCKS
N_DAYS = 1244                    # trading days of simulated EOD history (~4.9y); sized so
                                 # N_UNDERLYINGS * N_DAYS * N_H * N_M lands near 12.0M quotes
HORIZONS = [21, 42, 63, 126, 189, 252]   # trading days to expiry, ~1,2,3,6,9,12 months
MONEYNESS = [0.80, 0.86, 0.92, 0.98, 1.02, 1.08, 1.14, 1.20]  # 4 puts, 4 calls
N_H = len(HORIZONS)
N_M = len(MONEYNESS)
R_FLAT = 0.04  # flat risk-free rate assumption, disclosed limitation (no term structure)

# QuoteRecord: i4 underlying_id, i4 day_index, i4 expiry_day_index, i4 is_put,
# f4 strike, f4 S0, f4 r, f4 borrow, f4 mid_price, f4 sigma_true  (40 bytes)
RECORD_DTYPE = np.dtype([
    ("underlying_id", "<i4"), ("day_index", "<i4"), ("expiry_day_index", "<i4"),
    ("is_put", "<i4"), ("strike", "<f4"), ("S0", "<f4"), ("r", "<f4"),
    ("borrow", "<f4"), ("mid_price", "<f4"), ("sigma_true", "<f4"),
])
assert RECORD_DTYPE.itemsize == 40


def build_universe(rng: np.random.Generator):
    """Per-underlying parameters. Stocks 0..199, index == 200."""
    sigma_true = np.empty(N_UNDERLYINGS)
    div_yield = np.empty(N_UNDERLYINGS)
    borrow = np.empty(N_UNDERLYINGS)
    s0_init = np.empty(N_UNDERLYINGS)

    sigma_true[:N_STOCKS] = rng.uniform(0.15, 0.45, N_STOCKS)
    # Lognormal-ish right tail so a genuine top decile of high-dividend names
    # exists, not a uniform spread: most names pay modestly, a real top
    # decile (utilities/telecom-style) pays a lot more.
    div_yield[:N_STOCKS] = np.clip(rng.lognormal(mean=np.log(0.018), sigma=0.75, size=N_STOCKS),
                                    0.0, 0.09)
    borrow[:N_STOCKS] = rng.uniform(0.001, 0.015, N_STOCKS)
    s0_init[:N_STOCKS] = rng.uniform(40.0, 200.0, N_STOCKS)

    # Index: low vol, no dividends, near-zero borrow.
    sigma_true[N_STOCKS] = 0.15
    div_yield[N_STOCKS] = 0.0
    borrow[N_STOCKS] = 0.0005
    s0_init[N_STOCKS] = 4200.0

    return sigma_true, div_yield, borrow, s0_init


def build_dividend_schedule(div_yield: np.ndarray, s0_init: np.ndarray) -> list[tuple[int, int, float]]:
    """Fixed quarterly discrete dollar dividends, amount fixed at the
    underlying's starting price (disclosed simplification: dividends do not
    grow or get re-based to a later spot during the run)."""
    div_days = list(range(63, N_DAYS, 63))  # quarterly, scaled to the full simulated history
    rows = []
    for u in range(N_UNDERLYINGS):
        if div_yield[u] <= 0.0:
            continue
        amt = div_yield[u] * s0_init[u] / 4.0
        for d in div_days:
            rows.append((u, d, amt))
    return rows


def simulate_s0_paths(rng: np.random.Generator, sigma_true: np.ndarray, s0_init: np.ndarray) -> np.ndarray:
    """Daily GBM paths, shape (N_UNDERLYINGS, N_DAYS)."""
    z = rng.standard_normal((N_UNDERLYINGS, N_DAYS))
    daily_vol = sigma_true[:, None] / np.sqrt(252.0)
    log_ret = -0.5 * daily_vol**2 + daily_vol * z
    log_path = np.cumsum(log_ret, axis=1)
    return s0_init[:, None] * np.exp(log_path)


def step1():
    rng = np.random.default_rng(SEED)
    sigma_true, div_yield, borrow, s0_init = build_universe(rng)
    s0_paths = simulate_s0_paths(rng, sigma_true, s0_init)
    div_rows = build_dividend_schedule(div_yield, s0_init)

    np.savez(DATA / "universe.npz", sigma_true=sigma_true, div_yield=div_yield,
             borrow=borrow, s0_init=s0_init, s0_paths=s0_paths)

    with open(DATA / "divs.csv", "w") as f:
        f.write("underlying_id,div_day_index,amount\n")
        for u, d, amt in div_rows:
            f.write(f"{u},{d},{amt:.6f}\n")

    with open(DATA / "universe.csv", "w") as f:
        f.write("underlying_id,is_index,sigma_true,div_yield,borrow,s0_init\n")
        for u in range(N_UNDERLYINGS):
            f.write(f"{u},{1 if u == N_STOCKS else 0},{sigma_true[u]:.6f},{div_yield[u]:.6f},"
                    f"{borrow[u]:.6f},{s0_init[u]:.4f}\n")

    # Term-structured generation vol per (u, day, h): mild upward term
    # structure so total variance is monotone non-decreasing in expiry
    # BEFORE any violations are injected (violations are injected in step2,
    # directly on price, after the "true" price is computed from this vol).
    total_rows = N_UNDERLYINGS * N_DAYS * N_H * N_M
    rec = np.zeros(total_rows, dtype=RECORD_DTYPE)
    row = 0
    moneyness = np.array(MONEYNESS)
    is_put_row = (moneyness <= 1.0).astype(np.int32)
    for u in range(N_UNDERLYINGS):
        s0u = s0_paths[u]
        sigu = sigma_true[u]
        bor = borrow[u]
        for d in range(N_DAYS):
            s0 = s0u[d]
            strikes = s0 * moneyness
            for hi, h in enumerate(HORIZONS):
                term_vol = sigu * (1.0 + 0.03 * hi)  # mild upward term structure
                n = N_M
                sl = slice(row, row + n)
                rec["underlying_id"][sl] = u
                rec["day_index"][sl] = d
                rec["expiry_day_index"][sl] = d + h
                rec["is_put"][sl] = is_put_row
                rec["strike"][sl] = strikes.astype(np.float32)
                rec["S0"][sl] = np.float32(s0)
                rec["r"][sl] = np.float32(R_FLAT)
                rec["borrow"][sl] = np.float32(bor)
                rec["mid_price"][sl] = 0.0  # placeholder; filled by the C++ pricer
                rec["sigma_true"][sl] = np.float32(term_vol)
                row += n
    assert row == total_rows
    rec.tofile(DATA / "params.bin")
    print(f"step1: wrote {total_rows} params rows to {DATA/'params.bin'} "
          f"({N_UNDERLYINGS} underlyings x {N_DAYS} days x {N_H} expiries x {N_M} strikes)")


def step2():
    rng = np.random.default_rng(SEED + 1)
    rec = np.fromfile(DATA / "priced.bin", dtype=RECORD_DTYPE)
    n = len(rec)
    print(f"step2: loaded {n} priced rows")

    true_price = rec["mid_price"].astype(np.float64).copy()
    strike = rec["strike"].astype(np.float64)
    s0 = rec["S0"].astype(np.float64)
    is_put = rec["is_put"]

    noise = rng.normal(0.0, 0.002, n)
    mid = np.clip(true_price * (1.0 + noise), 0.01, None)
    spread_frac = rng.uniform(0.01, 0.04, n)

    # --- Inject a controlled, disclosed number of no-arbitrage violations ---
    # Row layout is a fixed nested order: u -> day -> h -> m (m in [0, N_M)).
    # This lets group membership be computed arithmetically instead of a
    # groupby, which matters at this row count.
    def row_index(u, d, h, m):
        return ((u * N_DAYS + d) * N_H + h) * N_M + m

    n_ee = 450
    n_bf = 700
    n_cal = 697
    log = []

    # Early-exercise lower bound: ask below intrinsic value. Restrict the
    # candidate pool to rows that are genuinely in the money (intrinsic >
    # $0.50) so the injected violation is unambiguous.
    # The grid only ever quotes the OTM side of each strike (is_put set by
    # moneyness <= 1.0), so no row is ever naturally in the money and
    # "ask < intrinsic" can never occur without a manufactured adversarial
    # case (the same pattern test_main.cpp already uses for a hand-built
    # SVI arbitrage violation). Push these specific rows' strikes into the
    # money so the rule has something real to catch.
    ee_idx = rng.choice(n, size=n_ee, replace=False)
    strike = strike.copy()
    strike[ee_idx] = np.where(is_put[ee_idx] == 1, s0[ee_idx] * 1.30, s0[ee_idx] * 0.70)
    rec["strike"][ee_idx] = strike[ee_idx].astype(np.float32)
    ee_intrinsic = np.where(is_put[ee_idx] == 1, strike[ee_idx] - s0[ee_idx], s0[ee_idx] - strike[ee_idx])

    # Butterfly: pick (u,day,h,side) groups, break convexity at the inner
    # strike of that side.
    bf_rows = []
    tries = 0
    while len(bf_rows) < n_bf and tries < n_bf * 5:
        tries += 1
        u = rng.integers(0, N_UNDERLYINGS)
        d = rng.integers(0, N_DAYS)
        h = rng.integers(0, N_H)
        side_m = int(rng.choice([1, 2, 5, 6]))  # inner strike of put side or call side
        bf_rows.append(row_index(u, d, h, side_m))
    bf_rows = np.array(bf_rows[:n_bf])

    # Calendar: pick (u,day,side-position) and a later horizon index,
    # breaking monotonicity against the immediately shorter horizon at the
    # same strike position.
    cal_rows = []
    tries = 0
    while len(cal_rows) < n_cal and tries < n_cal * 5:
        tries += 1
        u = rng.integers(0, N_UNDERLYINGS)
        d = rng.integers(0, N_DAYS)
        m = rng.integers(0, N_M)
        h = rng.integers(1, N_H)  # later horizon; violated against h-1
        cal_rows.append(row_index(u, d, h, m))
    cal_rows = np.array(cal_rows[:n_cal])

    bid = mid * (1.0 - spread_frac / 2.0)
    ask = mid * (1.0 + spread_frac / 2.0)

    # Apply early-exercise violations: ask strictly below intrinsic.
    ask[ee_idx] = ee_intrinsic * 0.85
    mid[ee_idx] = np.minimum(mid[ee_idx], ask[ee_idx])
    bid[ee_idx] = np.minimum(bid[ee_idx], ask[ee_idx] * 0.95)
    log.append(("early_exercise_lower_bound", len(ee_idx)))

    # Apply butterfly violations: slam the inner strike's mid well below
    # its two same-side neighbors (m-1, m+1), breaking discrete convexity.
    left = bf_rows - 1
    right = bf_rows + 1
    neighbor_min = np.minimum(mid[left], mid[right])
    mid[bf_rows] = neighbor_min * 0.45
    bid[bf_rows] = mid[bf_rows] * (1.0 - spread_frac[bf_rows] / 2.0)
    ask[bf_rows] = mid[bf_rows] * (1.0 + spread_frac[bf_rows] / 2.0)
    log.append(("butterfly_convexity", len(bf_rows)))

    # Apply calendar violations: force this (later) expiry's mid well below
    # the immediately shorter expiry's mid at the same strike position.
    shorter = cal_rows - N_M
    mid[cal_rows] = mid[shorter] * 0.55
    bid[cal_rows] = mid[cal_rows] * (1.0 - spread_frac[cal_rows] / 2.0)
    ask[cal_rows] = mid[cal_rows] * (1.0 + spread_frac[cal_rows] / 2.0)
    log.append(("calendar_monotonicity", len(cal_rows)))

    # --- Quarantine: re-check all three rules on the (now partly violated)
    # raw quote surface, arithmetically via the same group structure. A
    # quote failing any rule is logged with that rule and never inverted. ---
    flagged = np.zeros(n, dtype=bool)
    rule_of = np.zeros(n, dtype=np.int8)  # 0 none, 1 butterfly, 2 calendar, 3 early-exercise

    # Early exercise: ask < intrinsic - tol, checked on ALL rows.
    intrinsic_all = np.where(is_put == 1, np.maximum(strike - s0, 0.0), np.maximum(s0 - strike, 0.0))
    ee_fail = ask < (intrinsic_all - 1e-6)
    rule_of[ee_fail] = 3
    flagged |= ee_fail

    # Butterfly: central 2nd difference of mid over consecutive same-side
    # strikes (m=1,2 for puts; m=5,6 for calls) must be >= -eps.
    eps_bf = 0.20  # absolute dollar floor: noise on real quotes is not infinitesimal
    for m in (1, 2, 5, 6):
        base = np.arange(0, n, N_M)
        idx_l = base + (m - 1)
        idx_c = base + m
        idx_r = base + (m + 1)
        d2 = mid[idx_l] - 2.0 * mid[idx_c] + mid[idx_r]
        bad = d2 < -eps_bf
        rule_of[idx_c[bad]] = np.where(rule_of[idx_c[bad]] == 0, 1, rule_of[idx_c[bad]])
        flagged[idx_c[bad]] = True

    # Calendar: mid at horizon h must be >= mid at horizon h-1 (same strike
    # position), minus a small eps, for h in 1..N_H-1.
    eps_cal = 0.20  # same absolute dollar floor as the butterfly check
    for h in range(1, N_H):
        base = np.array([row_index(u, d, h, m) for u in range(0) for d in range(0) for m in range(0)])
    # (vectorized version, avoids the empty-loop placeholder above)
    all_idx = np.arange(n)
    h_of = (all_idx // N_M) % N_H
    for h in range(1, N_H):
        this_h = all_idx[h_of == h]
        prev_h = this_h - N_M
        bad = mid[this_h] < (mid[prev_h] - eps_cal)
        bad_idx = this_h[bad]
        rule_of[bad_idx] = np.where(rule_of[bad_idx] == 0, 2, rule_of[bad_idx])
        flagged[bad_idx] = True

    clean_mask = ~flagged
    n_flagged = int(flagged.sum())
    counts = {
        "butterfly_convexity": int((rule_of == 1).sum()),
        "calendar_monotonicity": int((rule_of == 2).sum()),
        "early_exercise_lower_bound": int((rule_of == 3).sum()),
    }

    out = rec.copy()
    out["mid_price"] = mid.astype(np.float32)
    out = out[clean_mask]
    out.tofile(DATA / "clean_quotes.bin")

    report = DATA / "quarantine_report.txt"
    with open(report, "w") as f:
        f.write(f"total raw quotes: {n}\n")
        f.write(f"quarantined (any rule): {n_flagged} ({100.0*n_flagged/n:.5f}%)\n")
        for k, v in counts.items():
            f.write(f"  {k}: {v}\n")
        f.write(f"clean quotes passed to inversion: {len(out)}\n")
    print(report.read_text())
    print(f"step2: wrote {len(out)} clean quotes to {DATA/'clean_quotes.bin'}")


def step3():
    import csv as _csv
    uni = np.loadtxt(DATA / "universe.csv", delimiter=",", skiprows=1)
    div_yield = {int(row[0]): row[3] for row in uni}

    RESULT_DTYPE = np.dtype([
        ("underlying_id", "<i4"), ("day_index", "<i4"), ("expiry_day_index", "<i4"),
        ("is_put", "<i4"), ("strike", "<f4"), ("S0", "<f4"), ("sigma_true", "<f4"),
        ("american_iv", "<f4"), ("american_ok", "<i4"), ("european_iv", "<f4"),
        ("european_ok", "<i4"),
    ])
    assert RESULT_DTYPE.itemsize == 44
    res = np.fromfile(DATA / "results.bin", dtype=RESULT_DTYPE)
    ok = (res["american_ok"] == 1) & (res["european_ok"] == 1)
    res = res[ok]
    print(f"step3: {len(res)} of {len(np.fromfile(DATA/'results.bin', dtype=RESULT_DTYPE))} "
          f"rows had both inversions converge")

    gap_vol_pts = (res["european_iv"].astype(np.float64) - res["american_iv"].astype(np.float64)) * 100.0

    # Isolate 25-delta-ish puts: is_put==1 and moneyness roughly 0.86-0.92
    # (the two OTM put strikes nearest a conventional 25-delta point in this
    # grid; see README for why an exact delta-matched strike was not solved
    # for separately).
    moneyness = res["strike"].astype(np.float64) / res["S0"].astype(np.float64)
    is_put25 = (res["is_put"] == 1) & (moneyness >= 0.84) & (moneyness <= 0.93)

    yields = np.array([div_yield.get(int(u), 0.0) for u in res["underlying_id"]])
    stock_mask = res["underlying_id"] < N_STOCKS
    decile_cut = np.quantile(yields[stock_mask & is_put25], 0.90) if (stock_mask & is_put25).any() else 0
    top_decile_mask = is_put25 & stock_mask & (yields >= decile_cut)

    median_gap_all_puts25 = float(np.median(gap_vol_pts[is_put25])) if is_put25.any() else float("nan")
    median_gap_top_decile = float(np.median(gap_vol_pts[top_decile_mask])) if top_decile_mask.any() else float("nan")

    out = DATA / "vol_gap_report.txt"
    with open(out, "w") as f:
        f.write(f"rows with both inversions converged: {len(res)}\n")
        f.write(f"25-delta-ish put rows (moneyness 0.84-0.93): {int(is_put25.sum())}\n")
        f.write(f"median (european_iv - american_iv) * 100, all 25-delta-ish puts: "
                f"{median_gap_all_puts25:.4f} vol points\n")
        f.write(f"dividend-yield decile cutoff (90th pct, stocks only): {decile_cut:.4f}\n")
        f.write(f"top-decile rows (25-delta-ish puts, stocks, top div yield decile): "
                f"{int(top_decile_mask.sum())}\n")
        f.write(f"median (european_iv - american_iv) * 100, top dividend decile: "
                f"{median_gap_top_decile:.4f} vol points\n")
    print(out.read_text())


if __name__ == "__main__":
    step = sys.argv[1] if len(sys.argv) > 1 else "step1"
    {"step1": step1, "step2": step2, "step3": step3}[step]()
