#include "baseclasses/Passes.h"

#include <algorithm>

namespace VP {

namespace {

bool contains(const std::vector<VkBuffer> &buffers, VkBuffer buffer) {
  return std::ranges::find(buffers, buffer) != buffers.end();
}

} // namespace

void Hazards::clear() {
  _written.clear();
  _read.clear();
}

// Every pass of a run counts after the barrier, since all of them run after it, so a
// read by the first still makes the next writer wait.
bool Hazards::before(std::span<const Pass> passes) {
  const auto written = [&](VkBuffer buffer) { return contains(_written, buffer); };
  const auto touched = [&](VkBuffer buffer) {
    return written(buffer) || contains(_read, buffer);
  };
  const bool wait = std::ranges::any_of(passes, [&](const Pass &pass) {
    return std::ranges::any_of(pass.reads, written) ||
           std::ranges::any_of(pass.writes, touched);
  });
  if (wait)
    clear();
  for (const Pass &pass : passes) {
    _written.insert(_written.end(), pass.writes.begin(), pass.writes.end());
    _read.insert(_read.end(), pass.reads.begin(), pass.reads.end());
  }
  return wait;
}

} // namespace VP
