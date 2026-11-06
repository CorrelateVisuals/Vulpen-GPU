#pragma once

#include "runtime/Operator.h"

// What nodes cost together, in milliseconds a frame, smoothed and as the last frame took
// it: whether any of them cooks or has a pass, and whether a pass of theirs went untimed.
// probe stats and the graph's heat sum them alike, so both tell the same cost.
namespace {

struct Weight {
  float cpu = 0.0f;
  float cpu_last = 0.0f;
  float gpu = 0.0f;
  float gpu_last = 0.0f;
  bool cooks = false;
  bool runs = false;
  bool untimed = false;
};

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
}

} // namespace
