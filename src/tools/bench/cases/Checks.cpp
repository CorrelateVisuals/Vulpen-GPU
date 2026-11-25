#include "runtime/Checks.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"
#include "tools/bench/Stage.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// runtime/Checks.cpp, function by function. A check runs on a node's Bound as the
// schedule binds it; here it runs again on the Bounds of real schedules, built as a run
// builds them on lavapipe (the wave project's two dispatches, the triangle's draw), each
// Bound given one value at an edge its caller may hand it. A row is what the check left
// on the Bound, its errors and warnings; the Bound gets back what it had after each row.

namespace VP::bench {

struct ScheduleBound {
  using type = std::vector<Bound> Schedule::*;
  friend constexpr type reach(ScheduleBound);
};
template struct Reach<ScheduleBound, &Schedule::_bound>;
struct CheckStages {
  using type = void (Schedule::*)(Bound &) const;
  friend constexpr type reach(CheckStages);
};
template struct Reach<CheckStages, &Schedule::check_stages>;

namespace {

constexpr std::uint32_t most = std::numeric_limits<std::uint32_t>::max();

// check_stages on the Bound with one value changed, and what it left there.
template <class Change>
std::string stages_checked(const Schedule &schedule, Bound &bound, const Change &change) {
  const std::vector<std::string> errors = std::exchange(bound.errors, {});
  const std::vector<std::string> warnings = std::exchange(bound.warnings, {});
  const std::uint32_t block_size = bound.block_size;
  const std::uint32_t invocations = bound.invocations;
  change(bound);
  std::string said;
  try {
    (schedule.*reach(CheckStages{}))(bound);
    said = std::format("errors {}; warnings {}",
                       escaped(joined(bound.errors, "; ")),
                       escaped(joined(bound.warnings, "; ")));
  } catch (...) {
    said = thrown();
  }
  bound.errors = errors;
  bound.warnings = warnings;
  bound.block_size = block_size;
  bound.invocations = invocations;
  return said;
}

// Each Bound of the project's schedules: its pass block at each size around what Vulkan
// guarantees (16,384 bytes) and what lavapipe holds (65,536), and a dispatch's count
// around its workgroup's size and the most workgroups lavapipe dispatches (65,535).
void stages_of(Rows &rows, Stage &stage, const std::filesystem::path &project) {
  Session session(stage, debug_mirror(rows));
  rows.quiet([&] {
    session.start(stage, rows.repo() / project);
    return 0;
  });
  for (Schedule *const schedule : session.views->schedules())
    for (Bound &bound : schedule->*reach(ScheduleBound{})) {
      const std::string at =
          std::format("{} node {}", project.generic_string(), bound.node->name);
      const std::vector<std::uint32_t> sizes{
          0, bound.block_size, 16384, 65536, 65537, most};
      for (const std::uint32_t size : sizes) {
        const auto sized = [&](Bound &into) { into.block_size = size; };
        rows.row(std::format("{}, block_size {} -> {}",
                             at,
                             size,
                             stages_checked(*schedule, bound, sized)));
      }
      if (!bound.node->counts_invocations() || bound.shaders.empty())
        continue;
      const std::uint32_t group = bound.shaders.front().workgroup_size()[0];
      const std::vector<std::uint32_t> counts{
          0, group, group + 1, bound.invocations, group * 65535, group * 65536, most};
      for (const std::uint32_t count : counts) {
        const auto counted = [&](Bound &into) { into.invocations = count; };
        rows.row(std::format("{}, invocations {} of local_size_x {} -> {}",
                             at,
                             count,
                             group,
                             stages_checked(*schedule, bound, counted)));
      }
    }
  rows.quiet([&] {
    session.views.reset();
    return 0;
  });
}

} // namespace

void cases(Bench &bench) {
  bench.check("Checks.cpp/Schedule::check_stages", [](Rows &rows) {
    on_window(rows, [&](Gpu &, const Window &window) {
      auto stage = rows.quiet([&] { return std::make_unique<Stage>(window); });
      for (const char *const project :
           {"projects/wave/view.vlp", "projects/triangles/triangle/view.vlp"})
        stages_of(rows, *stage, project);
      rows.quiet([&] {
        stage.reset();
        return 0;
      });
    });
  });
  carries_layer(bench, "Checks.cpp/Schedule::check_stages");
  takes_the_main_thread(bench, "Checks.cpp/Schedule::check_stages");
  bench.leaves_out("Checks.cpp/Schedule::check_stages",
                   "odd",
                   "the odd build lays its containers out otherwise (_GLIBCXX_DEBUG), so "
                   "the debug build's node modules do not load into it");
}

} // namespace VP::bench
