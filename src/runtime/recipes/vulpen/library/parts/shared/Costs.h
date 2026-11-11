#pragma once

#include "runtime/Operator.h"

// What nodes cost together, in milliseconds a frame, smoothed and as the last frame took
// it: whether any of them cooks or has a pass, and whether a pass of theirs went untimed;
// and why one of them is left out, one failing before one that only waits for the live
// build, or with none left out, what one warns of, so the graph's box shows it. probe
// stats and the graph's heat sum them alike, so both tell the same cost.
namespace {

struct Weight {
  float cpu = 0.0f;
  float cpu_last = 0.0f;
  float gpu = 0.0f;
  float gpu_last = 0.0f;
  bool cooks = false;
  bool runs = false;
  bool untimed = false;
  std::string_view problem; // as its bind's first error names it; empty for none
  bool waits = false;
  bool warns = false;
};

// How much a problem says, so a box holding several nodes tells the most: an error, then
// a warning, which asks something of a person, then a wait, which ends on its own.
int weight_of(std::string_view problem, bool waits, bool warns) {
  return problem.empty() ? 0 : waits ? 1 : warns ? 2 : 3;
}

// A node's time counts on the CPU while it cooks and on the GPU while its pass was timed.
void add(Weight &weight, const VP::Cost &cost) {
  if (cost.cooks) {
    weight.cpu += cost.cpu;
    weight.cpu_last += cost.cpu_last;
  }
  if (cost.timed) {
    weight.gpu += cost.gpu;
    weight.gpu_last += cost.gpu_last;
  }
  weight.cooks = weight.cooks || cost.cooks;
  weight.runs = weight.runs || cost.runs;
  weight.untimed = weight.untimed || (cost.runs && !cost.timed);
  if (weight_of(cost.problem, cost.waits, cost.warns) >
      weight_of(weight.problem, weight.waits, weight.warns)) {
    weight.problem = cost.problem;
    weight.waits = cost.waits;
    weight.warns = cost.warns;
  }
}

} // namespace
