#!/usr/bin/env bash
# Build and run Vulpen: ./run.sh [--release] [arguments for vulpen]
set -euo pipefail

preset=debug
if [[ ${1:-} == --release ]]; then preset=release; shift; fi
# vulpen's --log sets how much the build says too: all of it at debug, and at any other
# level nothing unless it fails (C09).
verbose=false
previous=
for argument in "$@"; do
  [[ $previous == --log && $argument == debug ]] && verbose=true
  previous=$argument
done

# CMake reads the presets from the current folder; vulpen itself runs from the
# caller's, since nothing in it may depend on the working directory.
root=$(dirname "$0")
build() { cd "$root" && cmake --preset "$preset" && cmake --build --preset "$preset" --parallel; }
if $verbose; then
  (build)
elif ! said=$(build 2>&1); then
  printf '%s\n' "$said" >&2
  exit 1
fi
exec "$root/out/build/$preset/vulpen" "$@"
