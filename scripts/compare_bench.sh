#!/usr/bin/env bash
# Builds the benchmark at the tag "baseline-list-orders" (std::list per level,
# one heap node per order) and at the current checkout, then runs them one
# after the other a few times so both see similar machine load.
set -euo pipefail
cd "$(dirname "$0")/.."

PAIRS="${PAIRS:-5}"
ROOT="$PWD"
BASE="$ROOT/build/baseline-src"

cleanup() { git worktree remove --force "$BASE" 2>/dev/null || true; }
trap cleanup EXIT
cleanup
git worktree add --detach "$BASE" baseline-list-orders >/dev/null

build() {  # source dir, build dir
    cmake -S "$1" -B "$2" -DCMAKE_BUILD_TYPE=Release -DMATCHBOOK_BUILD_TESTS=OFF >/dev/null
    cmake --build "$2" --target matchbook_bench -j >/dev/null
}
build "$BASE" "$ROOT/build/baseline"
build "$ROOT" "$ROOT/build/current"

for i in $(seq "$PAIRS"); do
    for v in baseline current; do
        printf '%-9s ' "$v"
        "$ROOT/build/$v/matchbook_bench" --runs 5 | grep -E 'median|latency' | tr '\n' ' '
        echo
    done
done
