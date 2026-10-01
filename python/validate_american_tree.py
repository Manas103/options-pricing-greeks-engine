"""Cross-validates the C++ american_cross_check CSV against the independent
pure-Python oracle in vol_store/oracle_american_tree.py. Same pattern as
validate_bs_grid.py: the C++ side alone cannot verify its own "ground
truth"; an independent implementation in a different language has to agree
with it closely for either to be trusted.

Usage: python validate_american_tree.py [path/to/american_cross_check.csv]
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vol_store.oracle_american_tree import price_american


def main():
    csv_path = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).resolve().parent.parent / "docs" / "american_cross_check.csv")
    rows = []
    with csv_path.open(newline="") as f:
        for row in csv.DictReader(f):
            rows.append(row)

    max_abs_diff = 0.0
    max_rel_diff = 0.0
    sum_abs_diff = 0.0
    n = len(rows)
    for row in rows:
        divs = []
        if int(row["n_divs"]) >= 1:
            divs.append((float(row["div1_time"]), float(row["div1_amt"])))
        if int(row["n_divs"]) >= 2:
            divs.append((float(row["div2_time"]), float(row["div2_amt"])))
        py_price = price_american(
            S0=float(row["S0"]), K=float(row["K"]), r=float(row["r"]),
            borrow=float(row["borrow"]), sigma=float(row["sigma"]), T=float(row["T"]),
            steps=int(row["steps"]), divs=divs, is_put=int(row["is_put"]) == 1)
        cpp_price = float(row["price"])
        abs_diff = abs(py_price - cpp_price)
        rel_diff = abs_diff / cpp_price * 100.0 if cpp_price > 1e-6 else 0.0
        max_abs_diff = max(max_abs_diff, abs_diff)
        max_rel_diff = max(max_rel_diff, rel_diff)
        sum_abs_diff += abs_diff

    print(f"loaded {n} American-tree cross-check cases from {csv_path}")
    print(f"C++ CRR tree vs independent pure-Python CRR oracle:")
    print(f"  max abs diff  = {max_abs_diff:.3e}")
    print(f"  mean abs diff = {sum_abs_diff / n:.3e}")
    print(f"  max rel diff  = {max_rel_diff:.6f} %")


if __name__ == "__main__":
    main()
