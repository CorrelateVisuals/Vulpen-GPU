#!/usr/bin/env bash
# Build and run Vulpen: ./run.sh [--release] [arguments for vulpen]
set -euo pipefail

preset=debug
if [[ ${1:-} == --release ]]; then preset=release; shift; fi

# CMake reads the presets from the current folder; vulpen itself runs from the
# caller's, since nothing in it may depend on the working directory.
root=$(dirname "$0")
(cd "$root" && cmake --preset "$preset" >/dev/null && cmake --build --preset "$preset")
exec "$root/out/build/$preset/vulpen" "$@"
