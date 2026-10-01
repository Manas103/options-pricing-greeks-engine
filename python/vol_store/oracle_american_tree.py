"""Independent, from-scratch, obviously-correct (slow) American CRR pricer,
written in pure Python without reusing any code or structure from
include/american_tree.hpp. This is the Python half of the two-language
cross-check validate_american_tree.py runs against
docs/american_cross_check.csv (produced by the C++ american_cross_check
binary at the same step count), the same pattern this repo already uses for
black_scholes.hpp vs black_scholes_ref.py.

Deliberately written with plain nested Python lists and loops rather than
numpy: slow, but every line is auditable against the CRR definition
directly, which is the point of an oracle.
"""
from __future__ import annotations

import math


def price_american(S0, K, r, borrow, sigma, T, steps, divs, is_put):
    """divs: list of (time, amount) with 0 < time <= T."""
    dt = T / steps
    u = math.exp(sigma * math.sqrt(dt))
    d = 1.0 / u
    growth = math.exp((r - borrow) * dt)
    p = (growth - d) / (u - d)
    disc = math.exp(-r * dt)

    def cum_div(t):
        return sum(amt for (tt, amt) in divs if tt <= t + 1e-12)

    # Terminal layer.
    values = []
    for j in range(steps + 1):
        s_raw = S0 * (u ** j) * (d ** (steps - j))
        s = s_raw - cum_div(T)
        intrinsic = max(K - s, 0.0) if is_put else max(s - K, 0.0)
        values.append(intrinsic)

    for i in range(steps - 1, -1, -1):
        t_i = i * dt
        div_i = cum_div(t_i)
        new_values = []
        for j in range(i + 1):
            cont = disc * (p * values[j + 1] + (1.0 - p) * values[j])
            s_raw = S0 * (u ** j) * (d ** (i - j))
            s = s_raw - div_i
            intrinsic = max(K - s, 0.0) if is_put else max(s - K, 0.0)
            new_values.append(max(cont, intrinsic))
        values = new_values
    return values[0]
