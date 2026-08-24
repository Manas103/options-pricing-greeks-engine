import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from black_scholes_ref import BSParams, call_delta, call_price, call_vega, put_price


def test_call_textbook_value():
    p = BSParams(S=100.0, K=100.0, r=0.05, sigma=0.20, T=1.0)
    assert abs(call_price(p) - 10.4506) < 1e-3


def test_put_call_parity():
    cases = [
        BSParams(100.0, 100.0, 0.05, 0.20, 1.0),
        BSParams(80.0, 100.0, 0.03, 0.35, 0.25),
        BSParams(120.0, 90.0, 0.01, 0.15, 2.0),
    ]
    for p in cases:
        lhs = call_price(p) - put_price(p)
        rhs = p.S - p.K * math.exp(-p.r * p.T)
        assert abs(lhs - rhs) < 1e-9


def test_delta_bounds():
    p = BSParams(S=100.0, K=100.0, r=0.05, sigma=0.20, T=1.0)
    d = call_delta(p)
    assert 0.0 <= d <= 1.0


def test_vega_positive():
    p = BSParams(S=100.0, K=100.0, r=0.05, sigma=0.20, T=1.0)
    assert call_vega(p) > 0.0


def test_deep_itm_call_converges_to_intrinsic():
    p = BSParams(S=200.0, K=100.0, r=0.03, sigma=0.20, T=0.01)
    intrinsic = p.S - p.K * math.exp(-p.r * p.T)
    assert abs(call_price(p) - intrinsic) < 0.5
