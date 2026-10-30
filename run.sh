#!/usr/bin/env bash
# Build and run Vulpen: ./run.sh [--release] [--cli | --ide | --<project> | view.vlp]
#                                 [vulpen's options]
set -euo pipefail
shopt -s lastpipe # a pipe's last command runs in this shell, so what it reads stays

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
# level its newest line, in place while it builds on a terminal, and all of it when it
# fails (C09), so a long build says what it builds and a quick one leaves nothing behind.
# Each run's log goes to docs/logs/, which git ignores, named for when it started, the
# newest 20 kept, so a run on another machine leaves behind what went wrong, a failed
# build's output with it; --log-file names another file, and that one alone is written.
app() { views=("$recipes/cli/view.vlp"); [[ $1 == --cli ]] || views+=("$recipes/ide/view.vlp"); }
arguments=()
opened=() # the targets of the projects it opens, as nodes.cmake names them
named=false
verbose=false
log=
while (($#)); do
  case $1 in
    --cli | --ide) app "$1"; arguments+=("${views[@]}"); named=true; shift ;;
    --log | --log-file | --frames | --fps | --first-frame | --source | --size | --restart)
      [[ $1 == --log && ${2:-} == debug ]] && verbose=true
      [[ $1 == --log-file ]] && log=${2:-}
      arguments+=("${@:1:2}")
      shift $(($# > 1 ? 2 : 1)) ;;
    --*)
      if [[ -f $projects/${1#--}/view.vlp ]]; then
        arguments+=("$projects/${1#--}/view.vlp"); named=true
        opened+=("vulpen_view_${1#--}")
      else
        arguments+=("$1") # vulpen names an option it does not know
      fi
      shift ;;
    *)
      arguments+=("$1"); named=true
      folder=$(cd "$(dirname -- "$1")" 2>/dev/null && pwd) || folder=
      [[ ${folder%/*} == "$projects" ]] && opened+=("vulpen_view_${folder##*/}")
      shift ;;
  esac
done
$named || { app "$default"; arguments=("${views[@]}" "${arguments[@]}"); }
if [[ -z $log ]]; then
  log=$root/docs/logs/run-$(date -u +%Y%m%dT%H%M%SZ).log
  runs=("$root"/docs/logs/run-*.log) # by name, so oldest first
  ((${#runs[@]} < 20)) || rm -f -- "${runs[@]:0:${#runs[@]}-19}"
  arguments+=(--log-file "$log")
fi
mkdir -p -- "$(dirname -- "$log")"

# CMake reads the presets from the current folder; vulpen itself runs from the
# caller's, since nothing in it may depend on the working directory. One build at a time
# on a tree, as two at once corrupt it: the build folder's build.lock, which a running
# vulpen's builds take too, held in a subshell, so it is let go before vulpen starts.
# vulpen's target builds the engine, the library and the apps, which vulpen needs; a
# project's builds its nodes, and only for the projects it opens, so one broken, or
# half typed when vulpen last quit, never keeps it from starting: its nodes name why, and
# the live build builds a project once it is opened or saved. The full build,
# cmake --build --preset debug, builds and checks every project, and the statistics.
build() (
  cd "$root" && mkdir -p "out/build/$preset"
  exec 9>"out/build/$preset/build.lock"
  if command -v flock >/dev/null; then flock 9; fi
  if [[ $1 == vulpen ]]; then cmake --preset "$preset" || exit; fi
  cmake --build --preset "$preset" --parallel --target "$1"
)
# Each line kept for a failure, and shown in place cut to the terminal's width, so it
# never wraps onto a row the next line would leave behind.
shown() {
  local line width
  width=$(stty size <&2 2>/dev/null | cut -d' ' -f2) || true
  ((${width:-0} > 1)) || width=80 # a terminal that gives no width
  while IFS= read -r line; do
    said+=$line$'\n'
    [[ -t 2 ]] && printf '\r\033[K%s' "${line:0:width-1}" >&2
  done
  [[ ! -t 2 ]] || printf '\r\033[K' >&2
}
said=
if $verbose; then
  (build vulpen) 2>&1 | tee -a "$log"
  for target in "${opened[@]}"; do (build "$target") 2>&1 | tee -a "$log" || true; done
elif ! build vulpen 2>&1 | shown; then
  printf '%s' "$said" | tee -a "$log" >&2
  exit 1
else
  for target in "${opened[@]}"; do
    said=
    build "$target" 2>&1 | shown && continue
    printf '%s' "$said" >>"$log"
    printf '%s failed to build, so its nodes say why as vulpen starts; the build said it in %s\n' \
      "${target#vulpen_view_}" "$log" >&2
  done
fi
exec "$root/out/build/$preset/vulpen" "${arguments[@]}"
