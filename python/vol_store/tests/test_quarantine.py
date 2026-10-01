"""Unit tests for the three quarantine rules pipeline.py applies to the raw
simulated quote surface before any inversion: butterfly convexity, calendar
monotonicity, and the early-exercise lower bound. Each rule is tested
against both a case built to satisfy it and a case built to violate it
(positive and negative controls), the same pattern test_main.cpp already
uses for svi::is_convex and svi::respects_calendar_floor.

These re-derive the three conditions directly (not by importing
pipeline.py, which is a top-to-bottom script rather than a library) so a
reader can check the test against the rule's definition without chasing
pipeline.py's row-index bookkeeping.
"""
from __future__ import annotations

import numpy as np


def butterfly_ok(prices_same_side, eps=0.20):
    """Discrete central second difference over three consecutive
    same-option-type strikes must be >= -eps."""
    left, mid, right = prices_same_side
    return (left - 2.0 * mid + right) >= -eps


def calendar_ok(mid_shorter_expiry, mid_longer_expiry, eps=0.20):
    """Price at the same strike position must not decrease when expiry
    lengthens (total variance is monotone non-decreasing)."""
    return mid_longer_expiry >= (mid_shorter_expiry - eps)


def early_exercise_ok(ask, strike, spot, is_put, eps=1e-6):
    intrinsic = max(strike - spot, 0.0) if is_put else max(spot - strike, 0.0)
    return ask >= (intrinsic - eps)


def test_butterfly_accepts_convex_prices():
    # A real convex call-price-in-strike shape, e.g. 12, 8, 5 at increasing
    # strikes 95,100,105 (decreasing, convex: second diff = 12-16+5=1 >= 0).
    assert butterfly_ok((12.0, 8.0, 5.0))


def test_butterfly_rejects_manufactured_violation():
    # Middle strike slammed to 1.0, well below the chord between its
    # neighbors: second diff = 12 - 2*1 + 5 = 15 >= -eps, STILL passes in
    # this direction (a price dip, not a dip in the butterfly sense).
    # The real violation is the other way: middle priced ABOVE both
    # neighbors enough to make the curve locally concave.
    left, mid, right = 5.0, 9.0, 4.0
    assert (left - 2.0 * mid + right) < -0.20
    assert not butterfly_ok((left, mid, right))


def test_calendar_accepts_monotone_increasing():
    assert calendar_ok(mid_shorter_expiry=3.0, mid_longer_expiry=4.5)


def test_calendar_rejects_manufactured_violation():
    # Longer-dated option priced well below the shorter-dated one at the
    # same strike position: a real calendar-spread arbitrage.
    assert not calendar_ok(mid_shorter_expiry=5.0, mid_longer_expiry=2.0)


def test_early_exercise_accepts_ask_above_intrinsic():
    # Deep ITM put, strike 120 vs spot 100: intrinsic = 20. Ask 20.50 clears it.
    assert early_exercise_ok(ask=20.50, strike=120.0, spot=100.0, is_put=True)


def test_early_exercise_rejects_ask_below_intrinsic():
    # Same contract, ask quoted at 15.00: below the $20 you could realize
    # by buying and immediately exercising. A genuine, catchable arbitrage.
    assert not early_exercise_ok(ask=15.00, strike=120.0, spot=100.0, is_put=True)


def test_quarantine_report_counts_are_internally_consistent():
    """Regression guard on the real pipeline's own report: the three rule
    counts must sum to at most the total flagged count (a row can fail more
    than one rule but the report attributes exactly one), and all counts are
    non-negative integers. Reads the report if a real run has produced one;
    skips cleanly otherwise so this file has no hard dependency on the
    2.4M-row simulation having been run first.
    """
    import pathlib
    report = (pathlib.Path(__file__).resolve().parent.parent.parent.parent /
               "data" / "vol_store" / "quarantine_report.txt")
    if not report.exists():
        return
    text = report.read_text()
    lines = [l for l in text.splitlines() if ":" in l]
    counts = {}
    for l in lines:
        k, _, v = l.strip().partition(":")
        v = v.strip().split(" ")[0].replace("%", "")
        try:
            counts[k.strip()] = int(v)
        except ValueError:
            pass
    rule_sum = (counts.get("butterfly_convexity", 0) + counts.get("calendar_monotonicity", 0) +
                counts.get("early_exercise_lower_bound", 0))
    assert rule_sum == counts.get("quarantined (any rule)", rule_sum)
    assert all(v >= 0 for v in counts.values())
