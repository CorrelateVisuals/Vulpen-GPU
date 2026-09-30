#include "baseclasses/Passes.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

// The barrier rule on made-up passes (V10). Synchronization validation cannot see
// accesses through buffer addresses, so nothing else checks the barriers between
// dispatches.

namespace {

// A handle nothing dereferences: the rule only compares them.
VkBuffer buffer(std::uint64_t id) {
  VkBuffer handle = VK_NULL_HANDLE;
  static_assert(sizeof handle == sizeof id);
  std::memcpy(&handle, &id, sizeof handle);
  return handle;
}

VP::Pass pass(std::vector<VkBuffer> reads, std::vector<VkBuffer> writes) {
  return {.reads = std::move(reads), .writes = std::move(writes)};
}

struct Case {
  const char *name;
  std::vector<VP::Pass> passes;
  std::vector<bool> barriers; // whether one goes before each pass
};

} // namespace

int main() {
  const VkBuffer x = buffer(1);
  const VkBuffer y = buffer(2);
  const std::vector<Case> cases{
      {"read after write", {pass({}, {x}), pass({x}, {})}, {false, true}},
      {"write after read", {pass({x}, {}), pass({}, {x})}, {false, true}},
      {"write after write", {pass({}, {x}), pass({}, {x})}, {false, true}},
      {"read after read", {pass({x}, {}), pass({x}, {})}, {false, false}},
      {"apart", {pass({}, {x}), pass({}, {y})}, {false, false}},
      {"read and write", {pass({x}, {x}), pass({x}, {y})}, {false, true}},
      {"after a barrier",
       {pass({}, {x}), pass({x}, {y}), pass({x}, {})},
       {false, true, false}},
      {"batched writers",
       {pass({}, {x}), pass({}, {y}), pass({y}, {})},
       {false, false, true}},
  };
  int failed = 0;
  VP::Hazards hazards;
  for (const Case &test : cases) {
    hazards.clear();
    for (std::size_t index = 0; index < test.passes.size(); ++index) {
      const bool placed = hazards.before(test.passes[index]);
      if (placed != test.barriers[index]) {
        std::printf(
            "%s: pass %zu %s a barrier\n", test.name, index, placed ? "gets" : "lacks");
        ++failed;
      }
    }
  }
  return failed == 0 ? 0 : 1;
}
