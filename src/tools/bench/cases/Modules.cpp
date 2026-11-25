#include "runtime/Modules.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Stage.h"
#include "tools/bench/Worlds.h"

#include <format>
#include <chrono>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

// runtime/Modules.cpp: the C++ of each node's folder, loaded from the debug build's
// mirror as a dev build loads it (bench/Stage.h's debug_mirror): what a module registers,
// what make and sole say of a name, whether one is built, and a copy of a module that the
// build writes again since it loaded, in a folder of the sandbox. A build that links the
// nodes in, as release does, finds them linked: its rows differ, and are left out.

namespace VP::bench {

namespace {

template <class F> std::string said(F &&call) {
  try {
    return call();
  } catch (...) {
    return thrown();
  }
}

} // namespace

void cases(Bench &bench) {
  // What each module of the build says of a name: its sole operator, and make of it and
  // of a name it lacks.
  bench.check("Modules.cpp/Modules::sole and make", [](Rows &rows) {
    const std::filesystem::path mirror = debug_mirror(rows);
    const std::vector<std::pair<std::string, std::string>> named{
        {"wave/wave", "Wave"},
        {"wave/probe", "Probe"},
        {"mistakes/wrong", "Wrong"},
        {"mistakes/scratch", "Scratch"},
        {"life/engine", "Engine"},
        {"wave/none", "None"},
        {"nothing/here", "Here"}};
    const std::string said_all = rows.quiet([&] {
      Modules modules;
      std::string text;
      for (const auto &[module, name] : named) {
        const std::filesystem::path folder = mirror / module;
        const std::string sole =
            said([&] { return escaped(modules.sole(module, folder)); });
        const std::string made = said([&] {
          return modules.make(module, folder, name) ? std::string("made")
                                                    : std::string("null");
        });
        const std::string missing =
            said([&] { return modules.make(module, folder, "Nope") ? "made" : "null"; });
        text += std::format("{}: built {}; sole {}; make {} {}; make Nope {}\n",
                            module,
                            modules.built(module, folder),
                            sole,
                            name,
                            made,
                            missing);
      }
      return text;
    });
    for (const auto piece : std::views::split(said_all, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(here(std::string(line), mirror)); // the mirror as <here>, in any build
  });

  // A module copied into the sandbox, loaded, and written again as a build writes it:
  // rewritten names it, unload lets it go, and the next make loads the new one.
  bench.check("Modules.cpp/a module the build writes again", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::filesystem::path module =
        debug_mirror(rows) / "wave" / "wave" / "module.so";
    if (!std::filesystem::exists(module)) {
      rows.row("no wave module: the debug build makes it");
      return;
    }
    const std::string said_all = rows.quiet([&] {
      Modules modules;
      Names names;
      std::string text;
      Files::copy(module, folder / "m" / "module.so");
      text += std::format("before a make: built_at set {}, rewritten {}\n",
                          modules.built_at("copy/m").has_value(),
                          show(modules.rewritten(), names));
      text += "make -> " + said([&] {
                return modules.make("copy/m", folder / "m", "Wave") ? std::string("made")
                                                                    : std::string("null");
              }) +
              "\n";
      text += std::format("after it: built_at set {}, rewritten {}\n",
                          modules.built_at("copy/m").has_value(),
                          show(modules.rewritten(), names));
      Files::remove(folder / "m" / "module.so");
      Files::copy(module, folder / "m" / "module.so");
      std::filesystem::last_write_time(folder / "m" / "module.so",
                                       std::filesystem::file_time_type::clock::now() +
                                           std::chrono::seconds(5));
      text +=
          std::format("written again: rewritten {}\n", show(modules.rewritten(), names));
      text += "make before unload -> " + said([&] {
                return modules.make("copy/m", folder / "m", "Wave") ? std::string("made")
                                                                    : std::string("null");
              }) +
              "\n";
      modules.unload("copy/m");
      text += "make after unload -> " + said([&] {
                return modules.make("copy/m", folder / "m", "Wave") ? std::string("made")
                                                                    : std::string("null");
              }) +
              "\n";
      text += std::format("after it: rewritten {}\n", show(modules.rewritten(), names));
      return text;
    });
    for (const auto piece : std::views::split(said_all, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(here(std::string(line), folder));
  });

  // A module whose entry carries another stamp, as one built against other engine headers
  // does: the build's stamp beside the bench is the bench's own, so a build makes the
  // module again, and loading it says so rather than asking for a restart.
  bench.check("Modules.cpp/a module built against other headers", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::filesystem::path module =
        debug_mirror(rows) / "wave" / "wave" / "module.so";
    if (!std::filesystem::exists(module)) {
      rows.row("no wave module: the debug build makes it");
      return;
    }
    const std::string entry = std::string("vp_module_") + VP_MODULE_STAMP;
    const std::string other = "vp_module_" + std::string(entry.size() - 10, 'x');
    std::string bytes = Files::text(module);
    for (std::size_t at = bytes.find(entry); at != std::string::npos;
         at = bytes.find(entry))
      bytes.replace(at, entry.size(), other);
    Files::save(folder / "old" / "module.so", bytes);
    const std::string made = rows.quiet([&] {
      Modules modules;
      return said([&] {
        return modules.make("copy/old", folder / "old", "Wave") ? std::string("made")
                                                                : std::string("null");
      });
    });
    rows.row("make of it -> " + here(made, folder));
  });

  // What a live build that ended says of the views: a failed one's nodes are in error.
  bench.check("Modules.cpp/Modules::builds_ended", [](Rows &rows) {
    Modules modules;
    const std::vector<std::string> built{"wave", "life"};
    const std::vector<std::string> failed{"life"};
    modules.builds_ended(built, failed);
    rows.row(
        std::format("after wave and life, life failed: wave failed {}, life failed {}",
                    modules.build_failed("wave"),
                    modules.build_failed("life")));
    const std::vector<std::string> again{"life"};
    modules.builds_ended(again, {});
    rows.row(std::format("after life again, none failed: life failed {}",
                         modules.build_failed("life")));
    modules.builds_ended({}, failed);
    rows.row(std::format("failed with none built: life failed {}",
                         modules.build_failed("life")));
  });

  for (const char *const id : {"Modules.cpp/Modules::sole and make",
                               "Modules.cpp/a module the build writes again",
                               "Modules.cpp/a module built against other headers"}) {
    bench.leaves_out(id,
                     "release",
                     "a release build links the nodes in, so it finds every "
                     "module linked and loads none");
    bench.leaves_out(id,
                     "odd",
                     "the odd build lays its containers out otherwise "
                     "(_GLIBCXX_DEBUG), so the debug build's node modules do not "
                     "load into it");
    bench.leaves_out(id,
                     "threads",
                     "a process loads a module once, through its one "
                     "Modules: a second at once finds the first's library "
                     "mapped, and refuses it as old code");
  }
}

} // namespace VP::bench
