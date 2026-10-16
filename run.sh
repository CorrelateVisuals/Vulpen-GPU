#!/usr/bin/env bash
# Build and run Vulpen: ./run.sh [--release] [--cli | --ide | --<project> | view.vlp]
#                                 [vulpen's options]
set -euo pipefail

default=--ide # what runs when no view is named; --cli runs the cli alone

preset=debug
if [[ ${1:-} == --release ]]; then preset=release; shift; fi
# Absolute, so the paths vulpen logs read whole, never through ./ (as run.ps1's).
root=$(cd "$(dirname "$0")" && pwd)
recipes=$root/src/runtime/recipes
projects=$root/projects
# An app's flag and a project's are the script's own and stand for views, so vulpen knows
# no app or project by name (V05): --cli for the cli's view.vlp, which vulpen starts
# with, and --ide for the cli's and the ide's, which vulpen then hosts beside the
# projects, as it hosts each view named after the first. vulpen's options come before a
# project, so one named like an option runs by its path; each takes a value, and any
# other word is a view.
# vulpen's --log sets how much the build says too: all of it at debug, and at any other
# level nothing unless it fails (C09).
app() { views=("$recipes/cli/view.vlp"); [[ $1 == --cli ]] || views+=("$recipes/ide/view.vlp"); }
arguments=()
named=false
verbose=false
while (($#)); do
  case $1 in
    --cli | --ide) app "$1"; arguments+=("${views[@]}"); named=true; shift ;;
    --log | --frames | --fps | --first-frame | --source | --size)
      [[ $1 == --log && ${2:-} == debug ]] && verbose=true
      arguments+=("${@:1:2}")
      shift $(($# > 1 ? 2 : 1)) ;;
    --*)
      if [[ -f $projects/${1#--}/view.vlp ]]; then
        arguments+=("$projects/${1#--}/view.vlp"); named=true
      else
        arguments+=("$1") # vulpen names an option it does not know
      fi
      shift ;;
    *) arguments+=("$1"); named=true; shift ;;
  esac
done
$named || { app "$default"; arguments=("${views[@]}" "${arguments[@]}"); }

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
