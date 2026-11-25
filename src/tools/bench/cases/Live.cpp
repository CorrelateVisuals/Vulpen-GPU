#include "runtime/Live.cpp"

#include "runtime/Manifest.h"
#include "runtime/Ports.h"
#include "tools/bench/Bench.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// runtime/Live.cpp: the live build off the frame thread. A build folder of the sandbox
// stands in for the build tree, its lists naming the views and the engine's files as the
// build writes them; cmake, finding no cache there, fails at once, so the scan, a build's
// end and quit restart's run as in a session, building nothing. A row says what the polls
// gave, each result but none, as how many polls a step takes follows the clock. What a
// failed build said reads as a person reads it: first_errors, over compilers' lines.

namespace VP::bench {

namespace {

// How long a step may take before its row says so: a cmake that fails at once, on a
// loaded machine.
constexpr auto step_most = std::chrono::seconds(60);
constexpr auto poll_interval = std::chrono::milliseconds(20);
// How long a step that should start nothing is watched.
constexpr auto quiet = std::chrono::milliseconds(400);
constexpr std::array ended{
    Live::Build::succeeded, Live::Build::failed, Live::Build::unsettled};
constexpr std::array starts{Live::Build::started};

std::string_view named(Live::Build build) {
  switch (build) {
    case Live::Build::none:
      return "none";
    case Live::Build::started:
      return "started";
    case Live::Build::succeeded:
      return "succeeded";
    case Live::Build::failed:
      return "failed";
    case Live::Build::unsettled:
      return "unsettled";
  }
  return "?";
}

// What the polls gave, but none, till one gave a result that ends the step, or nothing
// came in the time a step may take, or, given a quiet while, in that while: a step that
// should start nothing.
std::string polled(Live &live,
                   std::span<const Live::Build> until,
                   std::chrono::milliseconds quiet_for = {}) {
  std::string results;
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    const Live::Build build = live.poll();
    if (build != Live::Build::none)
      results.append(results.empty() ? "" : ", ").append(named(build));
    if (std::ranges::find(until, build) != until.end())
      return results;
    const auto took = std::chrono::steady_clock::now() - started;
    if (quiet_for.count() != 0 && results.empty() && took >= quiet_for)
      return "only none";
    if (took >= step_most)
      return results + ", and no more in a minute";
    std::this_thread::sleep_for(poll_interval);
  }
}

// A folder no row of this process used yet, holding the files given, as bench/Worlds.h's
// planted does, which this case cannot include: Worlds.h includes runtime/View.h, whose
// library_view meets Live.cpp's own of that name.
std::filesystem::path
planted(const Rows &rows, const std::vector<std::pair<std::string, std::string>> &files) {
  std::filesystem::path folder;
  for (std::size_t index = 0;; ++index) {
    folder = rows.root() / std::format("t{}", index);
    if (!std::filesystem::exists(folder))
      break;
  }
  Files::make(folder);
  for (const auto &[path, text] : files)
    Files::save(folder / path, text);
  return folder;
}

// A text with the folder as <here>, as Worlds.h's here() gives it.
std::string here(std::string text, const std::filesystem::path &folder) {
  const std::string from = folder.string();
  for (std::size_t at = text.find(from); at != std::string::npos;
       at = text.find(from, at))
    text.replace(at, from.size(), "<here>");
  return text;
}

// A file written now, a second ahead, so every scan after sees it newer than the last.
void written_now(const std::filesystem::path &file) {
  Files::save(file, "written\n");
  std::filesystem::last_write_time(
      file, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(1));
}

// The build tree's stand-in: the views it lists, the folders it reads them from, the
// engine's sources and vulpen, built after them.
struct Tree {
  std::filesystem::path top;
  std::filesystem::path build;
  std::filesystem::path views;
};

Tree tree_of(const Rows &rows) {
  Tree tree;
  tree.top = planted(rows,
                     {{"views/v/n/a.comp", "void main() {}"},
                      {"views/v/.vulpen/graph.ini", "x"},
                      {"views/w/m/b.comp", "void main() {}"},
                      {"views/x/k/c.comp", "void main() {}"},
                      {"engine/a.cpp", "int a;"}});
  tree.build = tree.top / "build";
  tree.views = tree.top / "views";
  Files::save(tree.build / VP_VIEWS_FILE, "v\nw\nlibrary\n");
  Files::save(tree.build / VP_READ_FILE, tree.views.string() + "\n");
  Files::save(tree.build / VP_ENGINE_SOURCES_FILE,
              (tree.top / "engine" / "a.cpp").string() + "\n");
  Files::save(tree.build / VP_ENGINE_FILE, "vulpen\n");
  std::filesystem::last_write_time(tree.top / "engine" / "a.cpp",
                                   std::filesystem::file_time_type::clock::now() -
                                       std::chrono::seconds(60));
  return tree;
}

// What the live build says once a step ended: the views it failed on and was for, and
// what it said, the tree as <here>.
std::string after(const Live &live, const Tree &tree) {
  return here(std::format("failed [{}]; built [{}]; building {}; errors {}",
                          joined(live.failed(), ", "),
                          joined(live.built(), ", "),
                          live.building(),
                          escaped(live.errors())),
              tree.top);
}

} // namespace

void cases(Bench &bench) {
  // Each file's first error, its file named from its view's folder; all of it when no
  // line is an error in a view's file.
  bench.check("Live.cpp/first_errors", [](Rows &rows) {
    const std::vector<std::string> folders{"/r/projects/wave/",
                                           "/r/projects/wave/inner/",
                                           "/r/projects/wave2/",
                                           "C:\\r\\p\\life\\",
                                           "/r/projects/lake/"};
    const std::vector<std::string> said{
        "",
        "Error: could not load cache\n",
        "/r/projects/wave/wave/Wave.cpp:12:5: error: 'x' was not declared in this "
        "scope\n",
        "/r/projects/wave/wave/Wave.cpp:12:5: error: one\n"
        "/r/projects/wave/wave/Wave.cpp:14:1: error: two\n"
        "/r/projects/wave/probe/Probe.cpp:3:1: error: three\n",
        "ERROR: /r/projects/wave/wave/wave.comp:7: '' : syntax error, unexpected "
        "IDENTIFIER\n"
        "ERROR: 1 compilation errors.  No code generated.\n",
        "ERROR: /r/projects/wave/wave/wave.comp:7: '' :   spaced\n",
        "/r/projects/wave/inner/n/N.cpp:1:1: error: the deepest folder names it\n",
        "/r/projects/wave2/w/W.cpp:2:2: error: not wave's\n",
        "C:\\r\\p\\life\\engine\\Engine.cpp(12): error C2065: 'x': undeclared "
        "identifier\n",
        "In file included from /r/projects/wave/wave/Wave.cpp:3:\n"
        "/r/src/runtime/Operator.h:100:3: error: an engine header's\n",
        "make[2]: *** [CMakeFiles/x.dir/build.make:76: x] Error 1\n"
        "/r/projects/wave/wave/Wave.cpp:1:1: warning: unused variable 'x'\n",
        "/r/projects/wave/wave/Wave.cpp error without a line\n",
        "/r/projects/wave/wave/Wave.cpp:1:1: error: crlf\r\n",
        "/r/projects/wave/:1: error: the folder itself\n",
        // An error whose words name a file of another view's folder, deeper or as deep:
        // the file it is in comes first in its line.
        "/r/projects/wave/w/W.cpp:1:1: error: cannot open /r/projects/wave/inner/x.h\n",
        "/r/projects/wave/a/A.cpp:1:1: error: cannot open /r/projects/wave/inner/x.h\n"
        "/r/projects/wave/b/B.cpp:2:2: error: cannot open /r/projects/wave/inner/x.h\n",
        "/r/projects/lake/l/L.cpp:1:1: error: in /r/projects/wave/w/W.h\n",
    };
    for (const std::string &text : said)
      rows.row(
          std::format("{} -> {}", escaped(text), escaped(first_errors(text, folders))));
    rows.row(std::format("no folders, one error -> {}",
                         escaped(first_errors("/r/a.cpp:1:1: error: x\n", {}))));
  });

  // A folder's newest time, as the scan reads it: a file's, a folder's, one hidden never;
  // the earliest there is where there is no folder. Each row's change stays for the next.
  bench.check("Live.cpp/newest", [](Rows &rows) {
    const std::filesystem::path folder =
        planted(rows, {{"a", "1"}, {"b/c", "2"}, {".hidden/d", "3"}, {"b/.e", "4"}});
    Files::make(folder / "f");
    const auto base =
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    const auto seconds = [&] {
      return std::chrono::duration_cast<std::chrono::seconds>(newest(folder) - base)
          .count();
    };
    for (const char *const each :
         {"a", "b/c", ".hidden/d", "b/.e", "f", "b", ".hidden", ""})
      std::filesystem::last_write_time(folder / each, base);
    rows.row(std::format("1. all at base: newest is base + {} s", seconds()));
    std::size_t step = 1;
    for (const auto &[file, ahead] :
         std::vector<std::pair<const char *, int>>{{".hidden/d", 50},
                                                   {".hidden", 45},
                                                   {"b/.e", 40},
                                                   {"b/c", 10},
                                                   {"f", 20},
                                                   {"a", 30}}) {
      std::filesystem::last_write_time(folder / file, base + std::chrono::seconds(ahead));
      rows.row(std::format(
          "{}. {} ahead {} s: newest is base + {} s", ++step, file, ahead, seconds()));
    }
    rows.row(
        std::format("{}. no such folder: the earliest time there is {}",
                    ++step,
                    newest(folder / "none") == std::filesystem::file_time_type::min()));
  });

  // A session's saves under the views, each a step polled to its end, then what the live
  // build says, numbered, as a step follows the one before.
  bench.check("Live.cpp/Live::poll", [](Rows &rows) {
    const Tree tree = tree_of(rows);
    Live live({{"v", tree.views / "v"}, {"w", tree.views / "w"}}, tree.build);
    std::size_t step = 0;
    // Its results taken before the state that follows them.
    const auto said = [&](std::string_view what, const std::string &results) {
      rows.row(
          std::format("{:02}. {} -> {}; {}", ++step, what, results, after(live, tree)));
    };
    said("nothing written", polled(live, ended, quiet));
    written_now(tree.views / "v" / ".vulpen" / "graph.ini");
    said("a file in v's .vulpen written", polled(live, ended, quiet));
    written_now(tree.views / "v" / "n" / ".a.comp.swp");
    said("a hidden file new in v's node folder", polled(live, ended, quiet));
    written_now(tree.views / "v" / "n" / "a.comp");
    said("a file of v written", polled(live, ended));
    // Written again once it started, before its end is read: the scan finds it moved.
    written_now(tree.views / "w" / "m" / "b.comp");
    said("a file of w written", polled(live, starts));
    written_now(tree.views / "w" / "m" / "b.comp");
    said("written again while it builds", polled(live, ended));
    said("once the files settle", polled(live, ended));
    // A view the build lists none of has no target: none of its own builds or fails.
    live.watch(
        {{"v", tree.views / "v"}, {"w", tree.views / "w"}, {"x", tree.views / "x"}});
    said("x watched, which the build has no target for", polled(live, ended));
    written_now(tree.views / "x" / "k" / "c.comp");
    said("a file of x written", polled(live, ended));
    rows.row(std::format("{:02}. targets [{}]", ++step, joined(live.targets(), ", ")));
    live.watch({{"w", tree.views / "w"}});
    rows.row(std::format("{:02}. v and x no longer watched: targets [{}]",
                         ++step,
                         joined(live.targets(), ", ")));
  });

  // An engine file written since vulpen was built is said once.
  bench.check("Live.cpp/Live::engine_changed", [](Rows &rows) {
    const Tree tree = tree_of(rows);
    // A second engine file, as old as the first, so two written at one time show which
    // of them the scan names.
    const std::filesystem::path first = tree.top / "engine" / "a.cpp";
    const std::filesystem::path second = tree.top / "engine" / "b.cpp";
    Files::save(second, "int b;");
    std::filesystem::last_write_time(second, std::filesystem::last_write_time(first));
    Files::save(tree.build / VP_ENGINE_SOURCES_FILE,
                first.string() + "\n" + second.string() + "\n");
    Live live({{"v", tree.views / "v"}}, tree.build);
    const auto changed = [&] {
      const std::filesystem::path file = live.engine_changed();
      return file.empty() ? std::string("none") : here(file.string(), tree.top);
    };
    polled(live, ended, quiet);
    rows.row("1. before: " + changed());
    written_now(tree.top / "engine" / "a.cpp");
    polled(live, ended, quiet);
    rows.row("2. written: " + changed());
    rows.row("3. asked again: " + changed());
    const auto at =
        std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2);
    for (const std::filesystem::path &file : {first, second}) {
      Files::save(file, "rewritten\n");
      std::filesystem::last_write_time(file, at);
    }
    polled(live, ended, quiet);
    rows.row("4. both written at one time: " + changed());
  });

  // Projects hosted from folders the build reads no views from, one after another, as
  // view load of projects in repositories of their own: each folder's line is written to
  // mounted-views once, and the line of a folder gone since goes as the next is written.
  bench.check("Live.cpp/Builder::mount", [](Rows &rows) {
    const std::filesystem::path top =
        planted(rows, {{"a/view.vlp", ""}, {"b/view.vlp", ""}, {"c/view.vlp", ""}});
    Files::save(top / "build" / VP_VIEWS_FILE, "library\n");
    Builder builder(top / "build", "mount.log");
    const auto hosted = [&](const std::string &name) {
      builder.start({std::format("{}{}", view_target, name)}, {{name, top / name}});
      const auto started = std::chrono::steady_clock::now();
      while (!builder.done() && std::chrono::steady_clock::now() - started < step_most)
        std::this_thread::sleep_for(poll_interval);
      return escaped(here(Files::text(top / "build" / VP_MOUNTED_FILE), top));
    };
    rows.row("1. a hosted -> " + hosted("a"));
    rows.row("2. b hosted, a still there -> " + hosted("b"));
    Files::remove(top / "a" / "view.vlp");
    rows.row("3. a gone, c hosted -> " + hosted("c"));
    rows.row("4. c hosted again -> " + hosted("c"));
  });

  // quit restart: asked through the port, it waits for no live build once that ended,
  // builds vulpen and the views, and says the build failed, the run going on. The port
  // and its log are made and dropped quietly, as their banners say nothing of it.
  bench.check("Live.cpp/Restart", [](Rows &rows) {
    struct Run {
      explicit Run(const std::filesystem::path &build)
          : ports(log, recipes, build / "views"), commands(log, ports, ports),
            restart(commands, log, build) {
        ports.listen(commands);
      }
      Log log{Level::error, "bench"};
      Recipes recipes;
      Ports ports;
      Commands commands;
      Restart restart;
    };
    const Tree tree = tree_of(rows);
    auto run = rows.quiet([&] { return std::make_unique<Run>(tree.build); });
    Restart &restart = run->restart;
    Live live({{"v", tree.views / "v"}}, tree.build);
    rows.row(std::format(
        "1. not asked: ready {}, asked {}", restart.ready(&live), restart.asked()));
    written_now(tree.views / "v" / "n" / "a.comp");
    rows.row("2. a file of v written -> " + polled(live, ended));
    rows.row("3. quit restart -> " + run->commands.run("quit restart"));
    rows.row("4. again -> " + run->commands.run("quit restart"));
    bool ready = false;
    bool built = false;
    {
      // The build waits for its lock while the case holds it, so the poll after its start
      // finds it running on any machine, as a build of vulpen runs in a session.
      const FileLock held(tree.build / build_lock);
      const bool starting = restart.ready(&live);
      built = restart.building();
      const bool waiting = restart.ready(&live);
      rows.row(
          std::format("4b. the poll that starts its build -> ready {}, building {}; the "
                      "next, the build waiting -> ready {}",
                      starting,
                      built,
                      waiting));
      ready = starting || waiting;
    }
    for (const auto started = std::chrono::steady_clock::now();
         std::chrono::steady_clock::now() - started < step_most;) {
      ready = restart.ready(&live);
      built = built || restart.building();
      if (ready || !restart.asked())
        break;
      std::this_thread::sleep_for(poll_interval);
    }
    rows.row(here(std::format("5. then ready {}; its build ran {}; asked {}, building {}",
                              ready,
                              built,
                              restart.asked(),
                              restart.building()),
                  tree.top));
    rows.row(std::format("6. without a live build, not asked -> ready {}",
                         restart.ready(nullptr)));
    // Asked in a run with no live build, as a release build's: it builds vulpen alone.
    rows.row("7. quit restart again -> " + run->commands.run("quit restart"));
    const bool first = restart.ready(nullptr);
    const bool building = restart.building();
    bool later = false;
    for (const auto started = std::chrono::steady_clock::now();
         std::chrono::steady_clock::now() - started < step_most;) {
      later = restart.ready(nullptr);
      if (later || !restart.asked())
        break;
      std::this_thread::sleep_for(poll_interval);
    }
    rows.row(
        std::format("8. with no live build: its first poll ready {}, building {}; then "
                    "ready {}, asked {}",
                    first,
                    building,
                    later,
                    restart.asked()));
    rows.quiet([&] {
      run.reset();
      return 0;
    });
  });
}

} // namespace VP::bench
