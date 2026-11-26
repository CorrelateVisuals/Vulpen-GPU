#include "runtime/Schedule.cpp"

#include "baseclasses/Passes.h"
#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"
#include "tools/bench/Stage.h"
#include "tools/bench/Worlds.h"

#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// runtime/Schedule.cpp, with Checks.cpp and Binder.cpp, the three files of one class:
// the schedule of every project of the tree, and of the fail-loud test's view, built as
// a run builds them (Runtime::start), headless on lavapipe (bench/Gpu.h), its nodes'
// code loaded from the debug build's mirror as a dev build loads it. A row per node: its
// problem, as the graph shows it, or what it binds; then its passes; then frames cooked
// and run, and each node's problem after them (A02, V10). The mistakes' rows are the
// refusals Checks words.

namespace VP::bench {

namespace {

std::string shown_cost(const Cost &cost) {
  if (cost.problem.empty())
    return std::format("{}: cooks {}, runs {}", cost.node, cost.cooks, cost.runs);
  return std::format("{}: {} {}",
                     cost.node,
                     cost.waits   ? "waits,"
                     : cost.warns ? "warns,"
                                  : "left out,",
                     escaped(cost.problem));
}

std::string shown_pass(const Pass &pass) {
  if (pass.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE)
    return std::format("dispatch of {} groups, reads {}, writes {}{}",
                       pass.groups,
                       pass.reads.size(),
                       pass.writes.size(),
                       pass.stores ? ", stores" : "");
  // A count the GPU adds up reaches the draw through its indirect command; a count
  // C++ writes, through the buffer's used length.
  const std::string instances =
      pass.indirect    ? std::string("instances counted on the GPU")
      : pass.instances ? std::string("counted instances")
                       : std::format("{} instances", pass.instance_count);
  return std::format("draw of {} vertices, {}{}{}",
                     pass.vertex_count,
                     instances,
                     pass.offscreen ? ", into an image" : ", into the window",
                     pass.pipeline ? "" : ", no pipeline");
}

// A port as the graph and the menus read it (Schedule::ports): its kind, which way, what
// it holds, whether a connection or its C++ joins it, and the loose ports alike.
std::string shown_port(const NodePort &port) {
  return std::format("{}.{}: {} {} {}{}{}",
                     port.node,
                     port.name,
                     port.kind,
                     port.writes ? "written" : "read",
                     port.holds.empty() ? "-" : port.holds,
                     port.joined ? ", joined" : "",
                     port.alike.empty() ? std::string()
                                        : ", alike " + joined(port.alike));
}

// The view's schedules as built, then after three frames cooked and run, a row each.
std::vector<std::string> scheduled(Stage &stage,
                                   const std::filesystem::path &mirror,
                                   const std::filesystem::path &manifest) {
  std::vector<std::string> said;
  Session session(stage, mirror);
  std::optional<Views> &views = session.views;
  try {
    session.start(stage, manifest);
  } catch (const std::exception &) {
    said.push_back("not built: " + thrown());
    return said;
  }
  for (const Schedule *const schedule : views->schedules()) {
    said.push_back(std::format(
        "a schedule, ok {}, {} nodes", schedule->ok(), schedule->costs().size()));
    for (const Cost &cost : schedule->costs())
      said.push_back("node " + shown_cost(cost));
    for (const NodePort &port : schedule->ports())
      said.push_back("port " + shown_port(port));
  }
  std::vector<Pass> passes;
  views->gather(passes);
  for (const Pass &pass : passes)
    said.push_back("pass " + shown_pass(pass));
  std::string ran = "done";
  try {
    for (std::uint64_t frame = 0; frame < 3; ++frame) {
      stage.engine.wait();
      views->rebuild();
      session.ports.frame();
      session.commands.frame();
      std::vector<VkBuffer> clears;
      std::vector<Copy> copies;
      for (Schedule *const schedule : views->schedules()) {
        schedule->cook(frame, static_cast<double>(frame) / 60, {64, 64});
        for (const VkBuffer buffer : schedule->take_clears())
          clears.push_back(buffer);
        for (const Copy &copy : schedule->take_copies())
          copies.push_back(copy);
      }
      passes.clear();
      views->gather(passes);
      stage.engine.run(clears,
                       copies,
                       passes,
                       frame,
                       static_cast<double>(frame) / 60,
                       {0, 0},
                       {64, 64});
    }
    stage.engine.wait();
  } catch (...) {
    ran = thrown();
  }
  said.push_back("three frames -> " + ran);
  for (const Schedule *const schedule : views->schedules())
    for (const Cost &cost : schedule->costs())
      said.push_back("after them, node " + shown_cost(cost));
  views.reset();
  return said;
}

// On a window of Xvfb's, as a run of a view that draws opens one, so its draws are
// scheduled into it as they are in a run.
void every_view(Rows &rows, const std::vector<std::filesystem::path> &manifests) {
  on_window(rows, [&](Gpu &, const Window &window) {
    const std::filesystem::path mirror = debug_mirror(rows);
    auto stage = rows.quiet([&] { return std::make_unique<Stage>(window); });
    for (const std::filesystem::path &manifest : manifests) {
      const std::vector<std::string> said =
          rows.quiet([&] { return scheduled(*stage, mirror, rows.repo() / manifest); });
      for (const std::string &line : said)
        rows.row(std::format("{}: {}", manifest.generic_string(), line));
    }
    rows.quiet([&] {
      stage.reset();
      return 0;
    });
  });
}

} // namespace

void cases(Bench &bench) {
  bench.check("Schedule.cpp/every project", [](Rows &rows) {
    std::vector<std::filesystem::path> projects;
    for (const std::filesystem::path &manifest : worlds::manifests(rows.repo()))
      if (manifest.generic_string().starts_with("projects/"))
        projects.push_back(manifest);
    every_view(rows, projects);
  });
  bench.check("Schedule.cpp/the fail-loud view", [](Rows &rows) {
    every_view(rows, {"src/tools/tests/mistakes/view.vlp"});
  });
  // Copies with a mistake more, each keeping its folder's name, so the debug build's
  // mirror has its code. The fail-loud view with four, each on a node of its own: an
  // image word nothing makes, an image connected into a buffer, a C++ object into an
  // operator that reads none of that name, and a Texture its operator fills connected
  // too. Wave with its connection gone, so a buffer written and one read are loose,
  // alike; triangle-compute's, so its screen samples no image while the raster stores
  // one; and its screen alone, sampling no image and with none alike.
  bench.check("Schedule.cpp/the fail-loud view, more mistakes", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const auto copied =
        [&](const std::filesystem::path &from,
            std::string_view name,
            const std::vector<std::pair<std::string_view, std::string_view>> &inserted,
            const std::vector<std::string_view> &cuts,
            std::string_view added) {
          copy_built(rows.repo() / from, folder / name);
          const std::filesystem::path manifest = folder / name / "view.vlp";
          std::string text = Files::text(manifest);
          // Each after the line it follows, as the view has it.
          for (const auto &[after, line] : inserted)
            if (const std::size_t at = text.find(after); at != std::string::npos)
              text.insert(at + after.size(), line);
          for (const std::string_view cut : cuts)
            if (const std::size_t at = text.find(cut); at != std::string::npos)
              text.erase(at, cut.size());
          Files::save(manifest, text + std::string(added));
          return manifest;
        };
    const std::vector<std::pair<std::string, std::filesystem::path>> views{
        {"mistakes+",
         copied(
             "src/tools/tests/mistakes",
             "mistakes",
             {{"[node \"pairs\"]\ninvocations = 64\n", "image       = extra=R8_UNORM\n"},
              {"from = give.count\nto   = take.count\n", "to   = look.count\n"}},
             {},
             "\n[connection \"into-buffer\"]\nfrom = stamp.canvas\nto   = sum.sums\n"
             "\n[connection \"filled-twice\"]\nfrom = scene.color\nto   = "
             "picture.picture\n")},
        {"wave-",
         copied("projects/wave",
                "wave",
                {},
                {"[connection \"values\"]\nfrom = wave.values\nto   = probe.values\n"},
                "")},
        {"triangle-compute-",
         copied(
             "projects/triangles/triangle-compute",
             "triangle-compute",
             {},
             {"[connection \"canvas\"]\nfrom = raster.canvas\nto   = screen.picture\n"},
             "")},
        {"triangle-compute, screen alone-",
         copied("projects/triangles/triangle-compute",
                "alone/triangle-compute",
                {},
                {"[connection \"canvas\"]\nfrom = raster.canvas\nto   = screen.picture\n",
                 "[node \"raster\"]\ninvocations = 65536\nparam       = "
                 "frames_per_turn=600\n"},
                "")}};
    on_window(rows, [&](Gpu &, const Window &window) {
      auto stage = rows.quiet([&] { return std::make_unique<Stage>(window); });
      for (const auto &[name, manifest] : views) {
        const std::vector<std::string> said =
            rows.quiet([&] { return scheduled(*stage, debug_mirror(rows), manifest); });
        for (const std::string &line : said)
          rows.row(here(std::format("{}: {}", name, line), folder));
      }
      rows.quiet([&] {
        stage.reset();
        return 0;
      });
    });
  });
  // A draw whose instances a dispatch counts on the GPU, look reading its image and seen
  // its depth: fail-loud's counted view, its four nodes alone copied under the fixture's
  // name, as fail-loud links them, so the debug build's mirror has their code. Its
  // passes say the count reaches the draw by an indirect command, and its frames run it
  // under the bench's validation.
  bench.check("Schedule.cpp/a draw counted on the GPU, its depth kept", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::filesystem::path view = folder / "mistakes";
    for (const char *const node : {"count", "crowd", "look", "seen"})
      copy_built(rows.repo() / "src/tools/tests/mistakes" / node, view / node);
    const std::filesystem::path manifest = view / "view.vlp";
    Files::save(manifest,
                "[manifest]\nversion = 1\n"
                "[node \"count\"]\ninvocations = 4\n"
                "[node \"crowd\"]\nvertex_count = 3\ninstance_count = drawn\n"
                "[node \"look\"]\noperator = Look\ninvocations = 64\n"
                "[node \"seen\"]\ninvocations = 64\n"
                "[connection \"counts\"]\nfrom = count.drawn\nto = crowd.drawn\n"
                "[connection \"crowded\"]\nfrom = crowd.color\nto = look.painted\n"
                "[connection \"deep\"]\nfrom = crowd.depth\nto = seen.depth\n");
    on_window(rows, [&](Gpu &, const Window &window) {
      auto stage = rows.quiet([&] { return std::make_unique<Stage>(window); });
      const std::vector<std::string> said =
          rows.quiet([&] { return scheduled(*stage, debug_mirror(rows), manifest); });
      for (const std::string &line : said)
        rows.row(here(line, folder));
      rows.quiet([&] {
        stage.reset();
        return 0;
      });
    });
  });

  // What a rebuild keeps of the schedule it replaces, which the log says at debug: a
  // buffer of the same size and memory with what it holds, a stored image of the same
  // size and format, an image the C++ fills, a node's pipeline while its code is the
  // same. Then frames at a size of none, of no height, and at a size again. A row each
  // line about memory or nodes.
  bench.check("Schedule.cpp/a rebuild keeps what it can", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    for (const char *const project : {"projects/wave",
                                      "projects/triangles/triangle-compute",
                                      "projects/triangles/triangle-cpp-raster",
                                      "src/tools/tests/mistakes"})
      copy_built(rows.repo() / project,
                 folder / std::filesystem::path(project).filename());
    on_window(rows, [&](Gpu &, const Window &window) {
      auto stage =
          rows.quiet([&] { return std::make_unique<Stage>(window, Level::debug); });
      for (const auto &[project, line] : std::vector<std::pair<std::string, std::string>>{
               {"wave", "node set probe log=warn"},
               {"triangle-compute", "node set screen log=warn"},
               {"triangle-cpp-raster", "node set screen log=warn"},
               {"mistakes", "node set paint log=warn"}}) {
        const std::vector<std::string> said = rows.quiet([&] {
          std::vector<std::string> lines;
          Session session(*stage, debug_mirror(rows));
          try {
            session.start(*stage, folder / project / "view.vlp");
          } catch (...) {
            lines.push_back("not built: " + thrown());
            return lines;
          }
          std::uint64_t frame = 0;
          const auto step =
              [&](std::string_view what, std::string_view typed, VkExtent2D size) {
                if (!typed.empty())
                  session.commands.send_typed(typed, (folder / project).string());
                stage->engine.wait();
                session.views->rebuild();
                for (Schedule *const schedule : session.views->schedules()) {
                  schedule->cook(frame, static_cast<double>(frame) / 60, size);
                  schedule->take_clears();
                  schedule->take_copies();
                }
                ++frame;
                session.commands.frame();
                for (const Logged &each : session.commands.log())
                  if (each.text.find("{mem}") != std::string::npos ||
                      each.text.find("{nod}") != std::string::npos)
                    lines.push_back(std::format("{} {}: {}", frame, what, each.text));
              };
          step("started, a frame at 64 by 64", {}, {64, 64});
          step(line, line, {64, 64});
          step("a frame at no size", {}, {0, 0});
          step("a frame at 64 by 0", {}, {64, 0});
          step("a frame at 64 by 64 again", {}, {64, 64});
          stage->engine.wait();
          return lines;
        });
        for (const std::string &each : said)
          rows.row(here(std::format("{} {}", project, each), folder));
      }
      rows.quiet([&] {
        stage.reset();
        return 0;
      });
    });
  });
  for (const char *const id :
       {"Schedule.cpp/every project",
        "Schedule.cpp/the fail-loud view",
        "Schedule.cpp/a rebuild keeps what it can",
        "Schedule.cpp/the fail-loud view, more mistakes",
        "Schedule.cpp/a draw counted on the GPU, its depth kept"}) {
    carries_layer(bench, id);
    takes_the_main_thread(bench, id);
    bench.leaves_out(id,
                     "odd",
                     "the odd build lays its containers out otherwise "
                     "(_GLIBCXX_DEBUG), so the debug build's node modules do not "
                     "load into it");
  }
}

} // namespace VP::bench
