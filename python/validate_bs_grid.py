"""Cross-validates the C++ benchmark's grid CSV.

Loads docs/bs_grid.csv (produced by the bench_bs_grid C++ binary, which
contains the C++ closed-form price, the MC price, and the C++ relative
error) and recomputes the closed-form price independently in Python
(black_scholes_ref.py, which shares no code with the C++ closed form). This
checks that the C++ "ground truth" itself is not silently wrong, which the
C++ side alone cannot verify. Also, optionally, plots the strike/maturity
error surface to docs/error_surface.png (headless, no display required).

Usage: python validate_bs_grid.py [path/to/bs_grid.csv]
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

from black_scholes_ref import BSParams, call_price

R = 0.03
SIGMA = 0.20


def load_grid(path: Path):
    rows = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append({k: float(v) for k, v in row.items()})
    return rows


def main():
    csv_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "docs" / "bs_grid.csv"
    rows = load_grid(csv_path)

    max_cpp_vs_py = 0.0
    sum_cpp_vs_py = 0.0
    max_mc_vs_py_rel = 0.0
    sum_mc_vs_py_rel = 0.0

    for row in rows:
        p = BSParams(S=100.0, K=row["K"], r=R, sigma=SIGMA, T=row["T"])
        py_price = call_price(p)
        cpp_price = row["bs_price"]
        mc_price = row["mc_price"]

        abs_diff_cpp_py = abs(cpp_price - py_price)
        max_cpp_vs_py = max(max_cpp_vs_py, abs_diff_cpp_py)
        sum_cpp_vs_py += abs_diff_cpp_py

        rel_err_mc_py = abs(mc_price - py_price) / py_price * 100.0 if py_price > 1e-8 else 0.0
        max_mc_vs_py_rel = max(max_mc_vs_py_rel, rel_err_mc_py)
        sum_mc_vs_py_rel += rel_err_mc_py

    n = len(rows)
    print(f"loaded {n} grid cells from {csv_path}")
    print(f"C++ closed-form vs Python (scipy) closed-form: max abs diff = {max_cpp_vs_py:.3e}, "
          f"mean abs diff = {sum_cpp_vs_py / n:.3e}")
    print(f"MC price vs Python closed-form: max relative error = {max_mc_vs_py_rel:.5f} %, "
          f"mean relative error = {sum_mc_vs_py_rel / n:.5f} %")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np

        strikes = sorted(set(r["K"] for r in rows))
        maturities = sorted(set(r["T"] for r in rows))
        k_index = {k: i for i, k in enumerate(strikes)}
        t_index = {t: i for i, t in enumerate(maturities)}
        grid = np.zeros((len(maturities), len(strikes)))
        for row in rows:
            grid[t_index[row["T"]], k_index[row["K"]]] = row["rel_err_pct"]

        fig, ax = plt.subplots(figsize=(7, 5))
        im = ax.imshow(grid, aspect="auto", origin="lower",
                        extent=[min(strikes), max(strikes), min(maturities), max(maturities)],
                        cmap="viridis")
        ax.set_xlabel("Strike")
        ax.set_ylabel("Maturity (years)")
        ax.set_title("MC vs closed-form Black-Scholes: relative error (%)")
        fig.colorbar(im, ax=ax, label="relative error (%)")
        out_png = csv_path.parent / "error_surface.png"
        fig.savefig(out_png, dpi=120, bbox_inches="tight")
        print(f"error surface plot written to {out_png}")
    except ImportError:
        print("matplotlib/numpy not available, skipping plot")


if __name__ == "__main__":
    main()
