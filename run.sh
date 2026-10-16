#!/usr/bin/env bash
# Build and run Vulpen: ./run.sh [--release] [--cli | --ide | view.vlp] [vulpen's options]
set -euo pipefail

default=--ide # what runs when no view is named; --cli runs the cli alone

preset=debug
if [[ ${1:-} == --release ]]; then preset=release; shift; fi
# Absolute, so the paths vulpen logs read whole, never through ./ (as run.ps1's).
root=$(cd "$(dirname "$0")" && pwd)
recipes=$root/src/runtime/recipes
# An app's flag is the script's own and stands for the app's view.vlp, so vulpen knows no
# app by name (V05). Each of vulpen's options takes a value, so any other word is a view.
# vulpen's --log sets how much the build says too: all of it at debug, and at any other
# level nothing unless it fails (C09).
arguments=()
named=false
verbose=false
while (($#)); do
  case $1 in
    --cli | --ide) arguments+=("$recipes/${1#--}/view.vlp"); named=true; shift ;;
    --*)
      [[ $1 == --log && ${2:-} == debug ]] && verbose=true
      arguments+=("${@:1:2}")
      shift $(($# > 1 ? 2 : 1)) ;;
    *) arguments+=("$1"); named=true; shift ;;
  esac
done
$named || arguments=("$recipes/${default#--}/view.vlp" "${arguments[@]}")

# CMake reads the presets from the current folder; vulpen itself runs from the
# caller's, since nothing in it may depend on the working directory.
build() { cd "$root" && cmake --preset "$preset" && cmake --build --preset "$preset" --parallel; }
if $verbose; then
  (build)
elif ! said=$(build 2>&1); then
  printf '%s\n' "$said" >&2
  exit 1
fi
exec "$root/out/build/$preset/vulpen" "${arguments[@]}"
