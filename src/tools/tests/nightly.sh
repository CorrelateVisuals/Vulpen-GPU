#!/usr/bin/env bash
# Every build preset and its tests, then the soaks and the long fuzz
# (docs/plans/tests.md). Runs everything, then exits 1 if anything failed.
set -uo pipefail

cd "$(dirname "$0")/../../.."
status=0
run() { "$@" || status=1; }
for preset in debug release asan tsan; do
  run cmake --preset "$preset"
  run cmake --build --preset "$preset" --parallel
  run ctest --preset "$preset"
done
run ctest --preset gpu-validation
run ctest --preset nightly
run ctest --preset asan-nightly
exit $status
