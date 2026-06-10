#!/bin/bash
# Run the benchmark suite against a running cluster.
# Usage: ./scripts/benchmark.sh [ops] [pipeline] [build_dir]
set -e

OPS="${1:-50000}"
PIPELINE="${2:-64}"
BUILD_DIR="${3:-./build}"
CLI="$BUILD_DIR/kvstore-cli"

if [ ! -x "$CLI" ]; then
    echo "CLI binary not found at $CLI"
    exit 1
fi

# Find the leader by asking node 1 (the CLI auto-redirects)
echo "Running BENCH $OPS (pipeline $PIPELINE) ..."
printf 'BENCH %d %d\nSTATUS\nQUIT\n' "$OPS" "$PIPELINE" | "$CLI" --port 6001
