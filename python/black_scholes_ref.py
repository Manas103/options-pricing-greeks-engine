"""Independent Black-Scholes reference implementation.

Deliberately does not share any code with the C++ closed form in
include/black_scholes.hpp. Its only purpose is to be a second, independent
check on the C++ "ground truth" so that ground truth is not trusted blindly.
Uses scipy.stats.norm rather than a hand-rolled erf, on the opposite side of
that same independence argument.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

from scipy.stats import norm


@dataclass
class BSParams:
    S: float
    K: float
    r: float
    sigma: float
    T: float


def call_price(p: BSParams) -> float:
    d1 = (math.log(p.S / p.K) + (p.r + 0.5 * p.sigma**2) * p.T) / (p.sigma * math.sqrt(p.T))
    d2 = d1 - p.sigma * math.sqrt(p.T)
    return p.S * norm.cdf(d1) - p.K * math.exp(-p.r * p.T) * norm.cdf(d2)


def put_price(p: BSParams) -> float:
    d1 = (math.log(p.S / p.K) + (p.r + 0.5 * p.sigma**2) * p.T) / (p.sigma * math.sqrt(p.T))
    d2 = d1 - p.sigma * math.sqrt(p.T)
    return p.K * math.exp(-p.r * p.T) * norm.cdf(-d2) - p.S * norm.cdf(-d1)


def call_delta(p: BSParams) -> float:
    d1 = (math.log(p.S / p.K) + (p.r + 0.5 * p.sigma**2) * p.T) / (p.sigma * math.sqrt(p.T))
    return norm.cdf(d1)


def call_vega(p: BSParams) -> float:
    d1 = (math.log(p.S / p.K) + (p.r + 0.5 * p.sigma**2) * p.T) / (p.sigma * math.sqrt(p.T))
    return p.S * norm.pdf(d1) * math.sqrt(p.T)


if __name__ == "__main__":
    p = BSParams(S=100.0, K=100.0, r=0.05, sigma=0.20, T=1.0)
    print(f"call price: {call_price(p):.6f}")
    print(f"put price:  {put_price(p):.6f}")
    print(f"call delta: {call_delta(p):.6f}")
    print(f"call vega:  {call_vega(p):.6f}")
