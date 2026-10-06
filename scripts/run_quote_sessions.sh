#!/usr/bin/env bash
# Runs N quote-engine sessions with distinct feed seeds, each followed by
# an offline replay of its own capture, and prints every live/replay
# checksum pair so a human (or docs/quote_engine_output.txt) can confirm
# they match. Intended to be run from the repo root inside WSL2.
set -euo pipefail
cd "$(dirname "$0")/.."

N_SESSIONS="${1:-3}"
N_TICKS="${2:-50000}"
TICK_US="${3:-50}"
BASE_SEED="${4:-3000}"
OUT_DIR="${5:-/tmp/quote_sessions}"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

for i in $(seq 1 "$N_SESSIONS"); do
    session_dir="$OUT_DIR/session_$i"
    mkdir -p "$session_dir"
    seed=$((BASE_SEED + i))

    ./build/quote_engine_live "$session_dir/session.cap" "$session_dir/live_report.txt" 1 \
        > "$session_dir/live_out.txt" 2>&1 &
    live_pid=$!

    sleep 1
    ./build/quote_feed_sender "$N_TICKS" "$TICK_US" "$seed" > "$session_dir/sender_out.txt" 2>&1
    wait "$live_pid"

    ./build/quote_engine_replay "$session_dir/session.cap" "$session_dir/replay_report.txt" \
        > "$session_dir/replay_out.txt" 2>&1

    echo "=== session $i (seed=$seed) ==="
    echo "--- sender ---"
    cat "$session_dir/sender_out.txt"
    echo "--- live ---"
    cat "$session_dir/live_report.txt"
    echo "--- replay ---"
    cat "$session_dir/replay_report.txt"
    live_sum=$(grep checksum "$session_dir/live_report.txt" | cut -d= -f2)
    replay_sum=$(grep checksum "$session_dir/replay_report.txt" | cut -d= -f2)
    if [ "$live_sum" = "$replay_sum" ]; then
        echo "MATCH: live and replay checksums are identical ($live_sum)"
    else
        echo "MISMATCH: live=$live_sum replay=$replay_sum"
    fi
    echo
done
