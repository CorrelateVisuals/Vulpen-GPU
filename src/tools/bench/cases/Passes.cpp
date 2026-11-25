#include "baseclasses/Passes.cpp"

#include "tools/bench/Bench.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

// baseclasses/Passes.cpp: where barriers go between passes (V10). Handles here are made
// up and never dereferenced, as the rule only compares them; src/tools/tests/Barriers.cpp
// holds the cases a person wrote, this the rule over every run the inputs make.

namespace VP::bench {

struct HazardsWritten {
  using type = std::vector<VkBuffer> Hazards::*;
  friend constexpr type reach(HazardsWritten);
};
template struct Reach<HazardsWritten, &Hazards::_written>;
struct HazardsRead {
  using type = std::vector<VkBuffer> Hazards::*;
  friend constexpr type reach(HazardsRead);
};
template struct Reach<HazardsRead, &Hazards::_read>;
struct HazardsStored {
  using type = bool Hazards::*;
  friend constexpr type reach(HazardsStored);
};
template struct Reach<HazardsStored, &Hazards::_stored>;

namespace {

constexpr std::uint64_t buffers = 3; // few, so passes share them

VkBuffer buffer(std::uint64_t id) {
  VkBuffer handle = VK_NULL_HANDLE;
  static_assert(sizeof handle == sizeof id);
  std::memcpy(&handle, &id, sizeof handle);
  return handle;
}

// A run's passes run with no barrier between them; a frame is runs, one after another.
using Run = std::vector<Pass>;
using Frame = std::vector<Run>;

// Whether a pass of later must wait for earlier: it reads what earlier wrote, writes what
// earlier read or wrote, or comes after a store into an image, whose readers the rule
// does not track.
bool hazard(const Run &earlier, const Run &later) {
  const auto touches = [](const std::vector<VkBuffer> &these,
                          const std::vector<VkBuffer> &those) {
    return std::ranges::any_of(these, [&](VkBuffer one) {
      return std::ranges::find(those, one) != those.end();
    });
  };
  return std::ranges::any_of(earlier, [&](const Pass &first) {
    return first.stores || std::ranges::any_of(later, [&](const Pass &second) {
             return touches(second.reads, first.writes) ||
                    touches(second.writes, first.writes) ||
                    touches(second.writes, first.reads);
           });
  });
}

std::vector<bool> barriers_of(const Frame &frame) {
  Hazards hazards;
  std::vector<bool> barriers;
  for (const Run &run : frame)
    barriers.push_back(hazards.before(run));
  return barriers;
}

} // namespace

template <> struct Gen<VkBuffer> {
  using type = VkBuffer;
  static std::vector<VkBuffer> edges() {
    return {buffer(1), buffer(2), buffer(3), VK_NULL_HANDLE};
  }
  static VkBuffer random(Random &random) {
    return buffer(1 + random.below(buffers));
  }
};

template <> struct Gen<Pass> {
  using type = Pass;
  static std::vector<Pass> edges() {
    return {{.reads = {buffer(1)}, .writes = {buffer(2)}},
            {},
            {.reads = {buffer(1)}},
            {.writes = {buffer(1)}},
            {.reads = {buffer(1)}, .writes = {buffer(1)}},
            {.stores = true},
            {.reads = {buffer(1), buffer(1)}, .writes = {buffer(2), buffer(3)}}};
  }
  static Pass random(Random &random) {
    Pass pass;
    for (std::uint64_t index = random.below(3); index > 0; --index)
      pass.reads.push_back(buffer(1 + random.below(buffers)));
    for (std::uint64_t index = random.below(3); index > 0; --index)
      pass.writes.push_back(buffer(1 + random.below(buffers)));
    pass.stores = random.below(8) == 0;
    return pass;
  }
};

template <> struct Shown<Pass> {
  static std::string text(const Pass &pass, Names &names) {
    std::string text = "{";
    if (!pass.reads.empty())
      text += "reads " + show(pass.reads, names);
    if (!pass.writes.empty())
      text +=
          std::string(text.size() > 1 ? " " : "") + "writes " + show(pass.writes, names);
    if (pass.stores)
      text += std::string(text.size() > 1 ? " " : "") + "stores";
    return text + "}";
  }
};

template <> struct Shown<Hazards> {
  static std::string text(const Hazards &hazards, Names &names) {
    return std::format("written {} read {} stored {}",
                       show(hazards.*reach(HazardsWritten{}), names),
                       show(hazards.*reach(HazardsRead{}), names),
                       show(hazards.*reach(HazardsStored{}), names));
  }
};

void cases(Bench &bench) {
  bench.sweep("Passes.cpp/contains", contains);

  // A frame's barriers, and what the rule holds after it, the last run's included.
  bench.sweep("Passes.cpp/Hazards::before", [](const Frame &frame) {
    Hazards hazards;
    std::vector<bool> barriers;
    for (const Run &run : frame)
      barriers.push_back(hazards.before(run));
    return std::pair{barriers, hazards};
  });
  bench.holds(
      "Passes.cpp/Hazards::before",
      "enough: a run waits, by a barrier since, for every run it has a hazard with",
      [](const Frame &frame) {
        const std::vector<bool> barriers = barriers_of(frame);
        for (std::size_t later = 0; later < frame.size(); ++later)
          for (std::size_t earlier = 0; earlier < later; ++earlier)
            if (hazard(frame[earlier], frame[later]) &&
                std::none_of(barriers.begin() + static_cast<std::ptrdiff_t>(earlier) + 1,
                             barriers.begin() + static_cast<std::ptrdiff_t>(later) + 1,
                             [](bool barrier) { return barrier; }))
              return false;
        return true;
      });
  bench.holds("Passes.cpp/Hazards::before",
              "needed: a barrier comes only for a hazard with a run since the one before",
              [](const Frame &frame) {
                const std::vector<bool> barriers = barriers_of(frame);
                std::size_t since = 0;
                for (std::size_t later = 0; later < frame.size(); ++later) {
                  if (!barriers[later])
                    continue;
                  bool any = false;
                  for (std::size_t earlier = since; earlier < later; ++earlier)
                    any = any || hazard(frame[earlier], frame[later]);
                  if (!any)
                    return false;
                  since = later;
                }
                return true;
              });
  bench.holds(
      "Passes.cpp/Hazards::clear", "after clear, nothing waits", [](const Frame &frame) {
        Hazards hazards;
        for (const Run &run : frame)
          hazards.before(run);
        hazards.clear();
        return (hazards.*reach(HazardsWritten{})).empty() &&
               (hazards.*reach(HazardsRead{})).empty() &&
               !(hazards.*reach(HazardsStored{}));
      });
}

} // namespace VP::bench
