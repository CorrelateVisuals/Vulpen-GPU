#include "runtime/Runtime.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Worlds.h"

#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

// runtime/Runtime.cpp: how a run reads its arguments (Runtime::parse), each list as a
// shell hands it, what it takes or the refusal, which names the argument and shows the
// usage (A02); the kind of GPU a view's .vulpen/gpu.ini names; the window's input as the
// feed hands it to the port, presses counted in a row; and whole runs, vulpen as its
// arguments start it, headless, over copies of the projects, with what each logs and
// the exit code it ends with.

namespace VP::bench {

struct RuntimeParse {
  template <class F> friend void reach(RuntimeParse, F &&);
};
template struct Reached<RuntimeParse, &VP::Runtime::parse>;

struct RuntimeRestarted {
  using type = VP::Runtime::Relaunch (VP::Runtime::*)(std::uint64_t);
  friend constexpr type reach(RuntimeRestarted);
};
template struct Reach<RuntimeRestarted, &VP::Runtime::restarted>;

namespace domain {
// The words after vulpen, as a shell splits a line.
struct Arguments {};
// A .vulpen/gpu.ini as gpu toggle writes it or a person edits it.
struct GpuChoice {};
} // namespace domain

template <> struct Gen<domain::Arguments> {
  using type = std::vector<std::string>;
  static std::vector<type> edges() {
    return {{"a.vlp"},
            {},
            {"a.vlp", "b.vlp", "c.vlp"},
            {"a.vlp", "--frames"},
            {"a.vlp", "--frames", "2"},
            {"a.vlp", "--frames", "-1"},
            {"a.vlp", "--frames", "1e3"},
            {"a.vlp", "--frames", "2", "--frames", "3"},
            {"a.vlp", "--frames=2"},
            {"a.vlp", "--first-frame", "18446744073709551615"},
            {"a.vlp", "--size", "1280x720"},
            {"a.vlp", "--size", "0x0"},
            {"a.vlp", "--size", "x"},
            {"a.vlp", "--size", "1280"},
            {"a.vlp", "--size", "1280x"},
            {"a.vlp", "--size", "1280x720x3"},
            {"a.vlp", "--size", "1280X720"},
            {"a.vlp", "--fps", "0"},
            {"a.vlp", "--fps", "4294967296"},
            {"a.vlp", "--log", "debug"},
            {"a.vlp", "--log", "DEBUG"},
            {"a.vlp", "--log", "nope"},
            {"a.vlp", "--log-file", ""},
            {"a.vlp", "--source", "s.txt", "--restart", "r.log"},
            {"a.vlp", "--nope", "x"},
            {"a.vlp", "-frames", "2"},
            {"a.vlp", "--"},
            {"--frames", "2"}};
  }
  static type random(Random &random) {
    const std::vector<type> all = edges();
    type mixed = all[random.below(all.size())];
    const type more = all[random.below(all.size())];
    mixed.insert(mixed.end(), more.begin(), more.end());
    return mixed;
  }
};

template <> struct Gen<domain::GpuChoice> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"prefer = discrete\n",
            "",
            "prefer=integrated",
            "  prefer  =  discrete  \n",
            "prefer = gpu\n",
            "prefer =\n",
            "# The kind of GPU vulpen takes first here; gpu toggle writes it.\n"
            "prefer = integrated\n",
            "prefer = discrete\nprefer = integrated\n",
            "preferred = discrete\n",
            "prefer discrete\n",
            "x = 1\nprefer = integrated\n",
            "PREFER = discrete\n",
            "prefer = Discrete\n",
            "prefer = discrete # the fast one\n",
            "prefer = discrete\r\n",
            "\xef\xbb\xbfprefer = discrete\n"};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())] + all[random.below(all.size())];
  }
};

namespace {

// An event as a node reads it.
std::string shown(const Event &event) {
  const char *const way = event.down ? "down" : "up";
  switch (event.kind) {
    case Event::Kind::key:
      return std::format("key {} {}", escaped(event.name), way);
    case Event::Kind::text:
      return "text " + escaped(event.name);
    case Event::Kind::pointer:
      return std::format(
          "pointer {},{}", number_text(event.at.x), number_text(event.at.y));
    case Event::Kind::button:
      return std::format("button {} {}{}",
                         escaped(event.name),
                         way,
                         event.presses == 0 ? std::string()
                                            : std::format(" x{}", event.presses));
    case Event::Kind::wheel:
      return std::format(
          "wheel {},{}", number_text(event.turn.x), number_text(event.turn.y));
    case Event::Kind::focus:
      return event.down ? "focus on" : "focus off";
    case Event::Kind::axis:
      return std::format("axis {} {},{}",
                         escaped(event.name),
                         number_text(event.at.x),
                         number_text(event.at.y));
  }
  return "an event of no kind";
}

// A run finds its build's views beside itself, as vulpen does: beside the bench, a link
// to the debug build's, whose modules and SPIR-V the other GPU cases load too, made once
// for every bench process of the build.
void mirror_beside(const Rows &rows) {
  const std::filesystem::path beside = Files::executable().parent_path() / "views";
  std::error_code none;
  if (!std::filesystem::exists(beside, none))
    std::filesystem::create_directory_symlink(
        rows.repo() / "out" / "build" / "debug" / "views", beside, none);
}

// vulpen <words> run here, its log at info written to a file of the folder: each line it
// logged, the time of each, the banner and what differs by machine or build left out:
// the device and its layers, and how long the frames took; then the exit code, and with
// restarts, what quit restart would start in its place had it come after the first frame.
std::vector<std::string> ran(const Rows &rows,
                             const std::filesystem::path &folder,
                             std::vector<std::string> words,
                             bool restarts) {
  mirror_beside(rows);
  const std::filesystem::path log = folder / "run.log";
  std::vector<std::string> held{"vulpen"};
  held.insert(held.end(), words.begin(), words.end());
  held.insert(held.end(), {"--log", "info", "--log-file", log.string()});
  std::vector<char *> argv;
  for (std::string &word : held)
    argv.push_back(word.data());
  std::string relaunch;
  const std::string code = rows.quiet([&] {
    try {
      Runtime runtime(argv, "bench");
      const int exit = runtime.run();
      if (restarts) {
        // Its replay goes beside the program, which the bench is: it is not read back,
        // as every bench process writes it there.
        const Runtime::Relaunch next = (runtime.*reach(RuntimeRestarted{}))(1);
        relaunch =
            "quit restart after a frame would run " + next.program.filename().string();
        for (const std::string &word : next.arguments)
          relaunch += " " + (word.ends_with("restart.log") ? "<replay>" : word);
      }
      return std::format("exit {}", exit);
    } catch (...) {
      return thrown();
    }
  });
  std::vector<std::string> lines;
  constexpr std::size_t time_column = 19; // " 26.11.17 19:40:43 "
  const std::string text = Files::text(log);
  for (const std::string_view line : lines_in(text)) {
    if (line.size() <= time_column)
      continue;
    std::string said(line.substr(time_column));
    const auto holds = [&](std::string_view part) {
      return said.find(part) != std::string::npos;
    };
    if (said.starts_with("+--") || holds("V U L P E N") ||
        holds("<< Correlate Visuals >>") || said.starts_with("{gpu} runs on ") ||
        said.starts_with("{gpu} Vulkan validation") || holds("meets the floor") ||
        said == "{run} vulpen bench")
      continue;
    if (said.starts_with("{run} ran "))
      said = said.substr(0, said.find(" in "));
    lines.push_back(here(said, folder));
  }
  lines.push_back(code);
  if (!relaunch.empty())
    lines.push_back(here(relaunch, folder));
  return lines;
}

// The options parse gives, as a row shows them.
std::string options_text(const auto &options) {
  std::string hosted;
  for (const std::filesystem::path &file : options.hosted)
    hosted.append(hosted.empty() ? "" : " ").append(file.generic_string());
  return std::format(
      "manifest {} hosted [{}] frames {} first {} fps {} size {}x{} log {} "
      "log-file {} source {} restart {}",
      escaped(options.manifest.generic_string()),
      hosted,
      options.frames,
      options.first_frame,
      options.fps,
      options.size.width,
      options.size.height,
      static_cast<int>(options.log),
      escaped(options.log_file.generic_string()),
      escaped(options.source.generic_string()),
      escaped(options.restart.generic_string()));
}

} // namespace

void cases(Bench &bench) {
  bench.sweep(
      "Runtime.cpp/Runtime::parse",
      [](const std::vector<std::string> &words) {
        std::vector<std::string> held{"vulpen"};
        held.insert(held.end(), words.begin(), words.end());
        std::vector<char *> argv;
        for (std::string &word : held)
          argv.push_back(word.data());
        try {
          std::string text;
          reach(RuntimeParse{}, [&](auto parse) { text = options_text(parse(argv)); });
          return text;
        } catch (const std::exception &failure) {
          // The first line of the refusal; the usage it shows after is the same for all.
          const std::string said = failure.what();
          return "throws " + escaped(said.substr(0, said.find('\n')));
        }
      },
      domain::Arguments{});

  // .vulpen/gpu.ini: the kind of GPU its prefer line names, or the refusal naming the
  // file (A02).
  bench.sweep(
      "Runtime.cpp/Preference::read",
      [](const Rows &rows, const std::string &text) {
        const std::filesystem::path file = rows.root() / "gpu.ini";
        Files::save(file, text);
        try {
          const std::string kind = Preference::read(file);
          return kind.empty() ? std::string("none named") : "takes " + escaped(kind);
        } catch (const std::exception &failure) {
          return "throws " + escaped(here(failure.what(), rows.root()));
        }
      },
      domain::GpuChoice{});

  bench.check("Runtime.cpp/Preference::read, no file", [](Rows &rows) {
    const std::string kind = Preference::read(rows.root() / "none" / "gpu.ini");
    rows.row(kind.empty() ? "a launch with no gpu.ini takes the fastest"
                          : "a launch with no gpu.ini takes " + kind);
  });

  // The window's input handed to the port, as a node reads it the frame after. A press of
  // the button pressed last, soon and near it, counts on, as a double click's second
  // does; another button between, a press farther than the reach, or later than the
  // interval, counts 1 again.
  bench.check("Runtime.cpp/Feed", [](Rows &rows) {
    const std::string said = rows.quiet([&] {
      Log log{Level::warn, "bench"};
      Recipes recipes;
      Ports ports(log, recipes, rows.root() / "mirror");
      Feed feed(ports);
      WindowInput &window = feed;
      const InputPort &input = ports;
      std::string text;
      const auto frame = [&](std::string_view what) {
        ports.frame();
        std::string read;
        for (const Event &event : input.events())
          read += (read.empty() ? "" : ", ") + shown(event);
        text += std::format("{}: {}\n", what, read.empty() ? "nothing" : read);
      };
      window.pointer(10, 10);
      window.button("left", true);
      frame("1 a press");
      window.button("left", false);
      window.button("left", true);
      frame("2 let go and pressed again where it was");
      window.button("left", false);
      window.button("left", true);
      frame("3 a third");
      window.pointer(16, 4);
      window.button("left", false);
      window.button("left", true);
      frame("4 a fourth, 6 pixels off either way");
      window.pointer(23, 4);
      window.button("left", false);
      window.button("left", true);
      frame("5 7 pixels off");
      window.button("right", true);
      window.button("right", false);
      window.button("left", true);
      frame("6 another button between");
      window.button("left", false);
      std::this_thread::sleep_for(press_interval + std::chrono::milliseconds(100));
      window.button("left", true);
      frame("7 past the interval");
      window.key("a", true);
      window.text("é");
      window.wheel(0, -1);
      window.focus(false);
      window.focus(true);
      window.key("a", false);
      frame("8 keys, text, the wheel and focus");
      frame("9 the frame after");
      return text;
    });
    for (const auto piece : std::views::split(said, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(std::string(line));
  });

  // Whole runs, each over copies of the projects in a folder of its own, a window opening
  // on bench.py's own display where a view draws into one: what each logs, numbered in
  // order, and how it exits. A script's lines run before the first frame and a restart's
  // after them, a failing one ending no run; the view vulpen starts with fails loud on
  // any error, as a gpu.ini naming no kind stops the run.
  bench.check("Runtime.cpp/a run", [](Rows &rows) {
    struct Scenario {
      std::string_view said;
      std::vector<std::string> projects; // copied into the folder, by their paths
      std::vector<std::string> files;    // planted there, "path=text"
      std::vector<std::string> words;    // after vulpen, <here> for the folder
      bool restarts = false;             // and what quit restart would start
    };
    const std::vector<Scenario> scenarios{
        {"no view named", {}, {}, {}},
        {"wave from frame 120, unpaced",
         {"wave"},
         {},
         {"<here>/wave/view.vlp", "--frames", "2", "--first-frame", "120", "--fps", "0"}},
        {"wave, restarted",
         {"wave"},
         {"script=param set wave amplitude 2\n"},
         {"<here>/wave/view.vlp",
          "--frames",
          "2",
          "--size",
          "64x48",
          "--source",
          "<here>/script"},
         true},
        {"wave, a script toggling the GPU",
         {"wave"},
         {"script=gpu toggle\n"},
         {"<here>/wave/view.vlp", "--frames", "1", "--source", "<here>/script"}},
        {"wave, a script that changes a param and asks to quit",
         {"wave"},
         {"script=param set wave amplitude 2\nquit\n"},
         {"<here>/wave/view.vlp", "--frames", "2", "--source", "<here>/script"}},
        {"wave, a script that quits force",
         {"wave"},
         {"script=param set wave amplitude 2\nquit force\n"},
         {"<here>/wave/view.vlp", "--frames", "2", "--source", "<here>/script"}},
        {"wave, a restart replayed",
         {"wave"},
         {"restart.log=param set wave amplitude 2\n"},
         {"<here>/wave/view.vlp", "--frames", "1", "--restart", "<here>/restart.log"}},
        {"wave, a restart line that fails",
         {"wave"},
         {"restart.log=param set nothere amplitude 2\n"},
         {"<here>/wave/view.vlp", "--frames", "1", "--restart", "<here>/restart.log"}},
        {"wave, a gpu.ini naming no kind",
         {"wave"},
         {"wave/.vulpen/gpu.ini=prefer = gpu\n"},
         {"<here>/wave/view.vlp", "--frames", "1"}},
        {"wave hosting triangle-compute",
         {"wave", "triangles/triangle-compute"},
         {},
         {"<here>/wave/view.vlp",
          "<here>/triangle-compute/view.vlp",
          "--frames",
          "1",
          "--size",
          "64x64"}},
        {"triangle",
         {"triangles/triangle"},
         {},
         {"<here>/triangle/view.vlp", "--frames", "1", "--size", "64x64"}},
        // A count of samples the GPU offers none of, and fullscreen while vsync is off,
        // each refused, which ends the run as a script's error does; then 4 samples a
        // pixel, which the next frame's window takes, and vsync.
        {"triangle, a script asking 3 samples a pixel",
         {"triangles/triangle"},
         {"script=window samples 3\n"},
         {"<here>/triangle/view.vlp",
          "--frames",
          "2",
          "--size",
          "64x64",
          "--source",
          "<here>/script"}},
        {"triangle, a script asking fullscreen without vsync",
         {"triangles/triangle"},
         {"script=window fullscreen on\n"},
         {"<here>/triangle/view.vlp",
          "--frames",
          "2",
          "--size",
          "64x64",
          "--source",
          "<here>/script"}},
        {"triangle, a script setting 4 samples a pixel and vsync",
         {"triangles/triangle"},
         {"script=window samples 4\nwindow vsync on\n"},
         {"<here>/triangle/view.vlp",
          "--frames",
          "2",
          "--size",
          "64x64",
          "--source",
          "<here>/script"}},
        // Its shaders never load, so its operator binds against none: what they would
        // hold is not known, and the node's one error is its count.
        {"triangle counting invocations",
         {"triangles/triangle"},
         {"triangle/view.vlp=[manifest]\nversion = 1\n\n[node \"triangle\"]\n"
          "invocations = 3\nparam       = frames_per_turn=600\n"},
         {"<here>/triangle/view.vlp", "--frames", "1", "--size", "64x64"}},
    };
    for (const Scenario &scenario : scenarios) {
      const std::filesystem::path folder = planted(rows, {});
      for (const std::string &project : scenario.projects)
        copy_built(rows.repo() / "projects" / project,
                   folder / std::filesystem::path(project).filename());
      for (const std::string &file : scenario.files) {
        const std::size_t equals = file.find('=');
        Files::save(folder / file.substr(0, equals), file.substr(equals + 1));
      }
      std::vector<std::string> words;
      for (std::string word : scenario.words) {
        if (word.starts_with("<here>"))
          word = folder.string() + word.substr(std::string_view("<here>").size());
        words.push_back(std::move(word));
      }
      const std::vector<std::string> lines = ran(rows, folder, words, scenario.restarts);
      for (std::size_t at = 0; at < lines.size(); ++at)
        rows.row(std::format("{}: {:02} {}", scenario.said, at + 1, lines[at]));
    }
  });
  bench.leaves_out(
      "Runtime.cpp/a run",
      "threads",
      "a run holds the process's std::cout and std::cerr for its nodes' prints, "
      "which two at once would take from each other");
  bench.leaves_out(
      "Runtime.cpp/a run",
      "release",
      "a release build links the nodes in and runs no live build, so its runs "
      "load and say otherwise");
}

} // namespace VP::bench
