"""Proves the point-in-time guarantee: querying a quote chain "as of" a
past date must return exactly what was knowable at that date, never
leaking a later restatement, even though the restatement lives in the same
table. Small and self-contained (does not depend on the 2.4M-row
simulated store existing on disk), so it runs fast and offline, same as
the rest of this repo's test suite. Skips cleanly if duckdb is not
installed, following the model-validation-alerting precedent for optional
dependencies.
"""
from __future__ import annotations

import pytest

duckdb = pytest.importorskip("duckdb")


def make_store():
    con = duckdb.connect(":memory:")
    con.execute("""
        CREATE TABLE quotes (
            underlying_id INTEGER, trading_day INTEGER, expiry_day INTEGER,
            strike DOUBLE, is_put INTEGER, mid DOUBLE, as_of DATE
        )
    """)
    return con


def as_of_query(con, underlying_id, trading_day, expiry_day, strike, is_put, as_of_date):
    """The official point-in-time read: among all rows for this exact
    contract, take the one with the latest as_of that is still <= the
    requested as_of_date. Never looks at a row restated after the fact."""
    row = con.execute("""
        SELECT mid FROM quotes
        WHERE underlying_id = ? AND trading_day = ? AND expiry_day = ? AND strike = ?
          AND is_put = ? AND as_of <= ?
        ORDER BY as_of DESC
        LIMIT 1
    """, [underlying_id, trading_day, expiry_day, strike, is_put, as_of_date]).fetchone()
    return row[0] if row else None


def test_as_of_query_unaffected_by_later_restatement():
    con = make_store()
    # Original EOD print on day 1.
    con.execute("INSERT INTO quotes VALUES (7, 1, 63, 100.0, 0, 5.25, '2024-01-02')")
    assert as_of_query(con, 7, 1, 63, 100.0, 0, "2024-01-02") == 5.25

    # A restatement/correction is appended (never mutated in place) five
    # days later, with a LATER as_of, for the SAME contract.
    con.execute("INSERT INTO quotes VALUES (7, 1, 63, 100.0, 0, 5.40, '2024-01-07')")

    # Querying "as of" the ORIGINAL date must be byte-for-byte unchanged:
    # the restatement did not exist yet as of 2024-01-02.
    assert as_of_query(con, 7, 1, 63, 100.0, 0, "2024-01-02") == 5.25

    # Querying "as of" a date between the original and the restatement
    # (e.g. the next trading day) must also still see only the original.
    assert as_of_query(con, 7, 1, 63, 100.0, 0, "2024-01-03") == 5.25

    # Querying "as of" the restatement's own date (or later) must now see
    # the corrected value.
    assert as_of_query(con, 7, 1, 63, 100.0, 0, "2024-01-07") == 5.40
    assert as_of_query(con, 7, 1, 63, 100.0, 0, "2024-01-31") == 5.40


def test_as_of_query_before_any_data_returns_none():
    con = make_store()
    con.execute("INSERT INTO quotes VALUES (7, 1, 63, 100.0, 0, 5.25, '2024-01-02')")
    assert as_of_query(con, 7, 1, 63, 100.0, 0, "2023-12-31") is None


def test_unrelated_contracts_do_not_leak_into_each_other():
    con = make_store()
    con.execute("INSERT INTO quotes VALUES (7, 1, 63, 100.0, 0, 5.25, '2024-01-02')")
    con.execute("INSERT INTO quotes VALUES (7, 1, 63, 105.0, 0, 3.10, '2024-01-02')")
    con.execute("INSERT INTO quotes VALUES (7, 1, 63, 105.0, 0, 3.90, '2024-01-07')")
    # The restatement on the 105 strike must not affect the 100 strike's
    # as-of read, even though both share every other key.
    assert as_of_query(con, 7, 1, 63, 100.0, 0, "2024-01-31") == 5.25
    assert as_of_query(con, 7, 1, 63, 105.0, 0, "2024-01-03") == 3.10
    assert as_of_query(con, 7, 1, 63, 105.0, 0, "2024-01-31") == 3.90
