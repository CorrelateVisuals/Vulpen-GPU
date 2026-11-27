#include "runtime/Ports.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <array>
#include <cstdio>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// runtime/Ports.cpp: the general ports a node reads besides commands (V05). The input a
// window or the input command makes, frame by frame, and what a hold of the pointer
// leaves the nodes outside it; the files the nodes open and the file port, on trees in
// the sandbox, with a mirror of the build beside them; the bytes a terminal sends for a
// key. What reads standard input (lines, keys) needs a terminal, which the bench gives
// no process: read only.

namespace VP::bench {

namespace domain {
// Bytes a terminal sends while a person types.
struct TerminalBytes {};
// A line of the input command.
struct InputLine {};
} // namespace domain

template <> struct Gen<domain::TerminalBytes> {
  using type = std::string;
  static std::vector<std::string> edges() {
    using namespace std::string_literals;
    return {"a",        "",         "abc",     "\r",     "\n",         "\t",
            "\x7f",     "\b",       "\x01",    "\x00"s,  "\x1b",       "\x1b[",
            "\x1b[A",   "\x1b[3~",  "\x1b[9~", "\x1bOH", "\x1bx",      "\x1b[1;5C",
            "ab\x1b[D", "\xc3\xa9", "\xc3",    "a\rb",   "\x1b\x1b[A", "\x1b[200~"};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())] + all[random.below(all.size())];
  }
};

template <> struct Gen<domain::InputLine> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"input pointer 1 2",        "input pointer nan nan",
            "input pointer inf -0",     "input pointer 1e40 0",
            "input pointer 1,5 2",      "input pointer x 2",
            "input key down a",         "input key down nokey",
            "input key down A",         "input key up f12",
            "input key down f13",       "input key down space",
            "input text a  b",          "input text é",
            "input button down left",   "input button down left 2",
            "input button down left 0", "input button down x",
            "input button up right",    "input wheel 1 -1",
            "input wheel nan 0",        "input focus on",
            "input focus off",          "input key down"};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())];
  }
};

template <> struct Shown<Moved> {
  static std::string text(const Moved &moved, Names &) {
    return std::format("{} -> {}",
                       escaped(moved.from),
                       moved.to.empty() ? std::string("gone") : escaped(moved.to));
  }
};

namespace {

// A port over the sandbox, its mirror at <here>/mirror, with the input command listened
// to on a port of its own.
struct Rig {
  explicit Rig(const std::filesystem::path &folder)
      : ports(log, recipes, folder / "mirror") {
    ports.listen(commands);
  }
  Log log{Level::warn, "bench"};
  Recipes recipes;
  Ports ports;
  SandboxFiles files;
  NoProcesses processes;
  Commands commands{log, files, processes};

  InputPort &input() {
    return ports;
  }
  FilePort &file_port() {
    return ports;
  }
  // The events a node of the view reads this frame; windowed: whether the view draws into
  // the window, rather than into an image a GUI shows.
  std::string read(std::string_view view, std::string_view node, bool windowed = true) {
    Names names;
    const InputPort &input = ports.input(view, node, windowed);
    return show(std::vector<Event>(input.events().begin(), input.events().end()), names);
  }
};

template <class F> std::string said(F &&call) {
  try {
    call();
    return "done";
  } catch (const std::exception &failure) {
    return "throws " + escaped(failure.what());
  }
}

} // namespace

void cases(Bench &bench) {
  bench.sweep(
      "Ports.cpp/key",
      [](const std::string &bytes) {
        std::vector<Event> events;
        std::size_t took = 0;
        if (!bytes.empty())
          took = key(bytes, events);
        Names names;
        return std::format("took {}: {}", took, show(events, names));
      },
      domain::TerminalBytes{});

  bench.sweep("Ports.cpp/number", number, domain::Word{});
  bench.sweep("Ports.cpp/checked_key", checked_key, domain::Word{});
  bench.sweep("Ports.cpp/checked_button", checked_button, domain::Word{});
  bench.sweep("Ports.cpp/presses_of", presses_of, domain::Word{});
  bench.sweep("Ports.cpp/normal", normal);

  // A line of the input command, then a frame: the events it made, as a window's.
  bench.sweep(
      "Ports.cpp/the input command",
      [](const Rows &rows, const std::string &line) {
        const std::filesystem::path folder = planted(rows, {});
        return rows.quiet([&] {
          Rig rig(folder);
          const std::string ran = said([&] { rig.commands.run(line); });
          rig.ports.frame();
          Names names;
          return std::format("{}; then {}; pointer ({},{})",
                             ran,
                             show(std::vector<Event>(rig.input().events().begin(),
                                                     rig.input().events().end()),
                                  names),
                             number_text(rig.input().pointer().x),
                             number_text(rig.input().pointer().y));
        });
      },
      domain::InputLine{});

  // Frames of input: moves in a row keep only the last, what came in reaches the next
  // frame and is gone the one after; a hold leaves the nodes outside it every event but
  // the presses and the wheel, a frame later; a hand gives the presses to another view.
  bench.check("Ports.cpp/frames of input", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::string said = rows.quiet([&] {
      Rig rig(folder);
      std::string text;
      const auto frame = [&](std::string_view what) {
        rig.ports.frame();
        text +=
            std::format("{}: menu.items.b reads {}; other reads {}; game's reads {}\n",
                        what,
                        rig.read("v", "menu.items.b"),
                        rig.read("v", "other"),
                        rig.read("game", "a"));
      };
      for (const char *const line : {"input pointer 1 1",
                                     "input pointer 2 2",
                                     "input button down left",
                                     "input pointer 3 3"})
        rig.commands.run(line);
      frame("three moves around a press");
      frame("the frame after");
      rig.ports.hold_pointer("v", "menu.items.a");
      rig.commands.run("input button down left");
      rig.commands.run("input wheel 0 1");
      rig.commands.run("input key down a");
      frame("held as the frame before cooked");
      text += std::format("held as the frame before cooked: menu.items2, named alike but "
                          "beside the menu, reads {}\n",
                          rig.read("v", "menu.items2"));
      text += std::format("held as the frame before cooked: a view drawn into an image, "
                          "outside the hold, reads {}\n",
                          rig.read("pane", "a", false));
      rig.ports.hand_pointer("game", "v", "menu.items.a", Ports::Shown{});
      rig.commands.run("input button down right");
      frame("handed to game");
      rig.commands.run("input button down middle");
      frame("let go");
      // A GUI hands the pointer each frame, with where it shows game's window: a picture
      // of 200 by 100 at 100,50 of a window of 800 by 400.
      const Ports::Shown shown{
          .at = {100.0f, 50.0f}, .size = {200.0f, 100.0f}, .window = {800.0f, 400.0f}};
      rig.ports.hand_pointer("game", "v", "menu.items.a", shown);
      rig.commands.run("input pointer 150 75");
      frame("handed over a picture, the pointer a quarter in");
      rig.ports.hand_pointer("game", "v", "menu.items.a", shown);
      rig.commands.run("input pointer 400 300");
      frame("handed over a picture, the pointer past it");
      rig.ports.unread("v", "a", "pointer");
      text += std::format("unread: v a pointer {}, v a keys {}\n",
                          rig.ports.was_unread("v", "a", "pointer"),
                          rig.ports.was_unread("v", "a", "keys"));
      rig.ports.frame();
      text += std::format("a frame after: v a pointer {}\n",
                          rig.ports.was_unread("v", "a", "pointer"));
      return text;
    });
    for (const auto piece : std::views::split(said, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(std::string(line));
  });

  // A gamepad's axes as lines: a stick, a trigger, and refusals past their ranges or of
  // a name no axis has; the window lines with no window, which wait for one; and a
  // frame's sound: the samples nodes add, mixed, clamped, a sample no number holds as
  // silence, handed on as played when the next frame starts, and no speakers opened
  // without a window.
  bench.check("Ports.cpp/axes, window lines and sound, with no window", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::string said = rows.quiet([&] {
      Rig rig(folder);
      std::string text;
      for (const char *const line : {"input axis gamepad_left 0.5 -1",
                                     "input axis gamepad_right_trigger 1 0",
                                     "input axis gamepad_left_trigger 0.5 0.5",
                                     "input axis gamepad_right_trigger -0.1 0",
                                     "input axis gamepad_left 1.5 0",
                                     "input axis gamepad_a 0 0",
                                     "input axis gamepad_left nan 0",
                                     "window title a  named window",
                                     "window fullscreen on",
                                     "window fullscreen off",
                                     "window pointer lock",
                                     "window pointer free"})
        try {
          text += std::format("{} -> {}\n", line, rig.commands.run(line));
        } catch (const std::exception &refused) {
          text += std::format("{} -> refused: {}\n", line, refused.what());
        }
      rig.ports.frame();
      text += std::format("the frame after: game's a reads {}\n", rig.read("game", "a"));
      SoundPort &sound = rig.ports;
      rig.ports.start_sound(4);
      text += std::format("a frame of 4: {} frames\n", sound.frames());
      const std::array<float, 10> first{0.25f,
                                        -0.5f,
                                        2.0f,
                                        -3.0f,
                                        std::numeric_limits<float>::quiet_NaN(),
                                        std::numeric_limits<float>::infinity(),
                                        0.5f,
                                        0.5f,
                                        9.0f,
                                        9.0f};
      sound.add(first);
      sound.add(std::array<float, 2>{0.25f, 0.25f});
      rig.ports.play_sound();
      rig.ports.start_sound(2);
      std::string played;
      for (const float sample : sound.played())
        played += std::format(" {}", sample);
      text += std::format("played, after the next frame started:{}; it holds {} frames\n",
                          played,
                          sound.frames());
      return text;
    });
    for (const auto piece : std::views::split(said, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(std::string(line));
  });

  // A node's file: shared by every node that opens it, compared with the disk once a
  // frame, empty while it is gone and read again once it is back.
  bench.check("Ports.cpp/files the nodes open", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {"a=1"});
    const std::string said = rows.quiet([&] {
      Rig rig(folder);
      FilePort &port = rig.file_port();
      std::string text;
      const auto show_text = [&](std::string_view what, File file) {
        std::string read;
        const std::string ran = VP::bench::said([&] { read = port.text(file); });
        text += std::format("{}: {}, reads {}\n", what, ran, escaped(read));
      };
      const File first = rig.ports.open(folder / "a");
      const File second = rig.ports.open(folder / "a");
      text +=
          std::format("opened twice: the same file {}\n", first.index == second.index);
      show_text("first read", first);
      Files::save(folder / "a", "2");
      show_text("changed on disk, the same frame", first);
      rig.ports.frame();
      show_text("the next frame", first);
      Files::remove(folder / "a");
      rig.ports.frame();
      show_text("gone", first);
      Files::save(folder / "a", "3");
      rig.ports.frame();
      show_text("back", first);
      port.save(first, "4");
      show_text("saved through the port", first);
      rig.ports.close(first);
      show_text("closed by one node", second);
      rig.ports.close(second);
      show_text("closed by both", second);
      const File none = rig.ports.open(folder / "nothing");
      show_text("a file that is not there", none);
      return text + "on disk: " + here(tree_of(folder), folder) + "\n";
    });
    for (const auto piece : std::views::split(said, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(here(std::string(line), folder));
  });

  // The file port over a tree with a view in it and a mirror of its build: what each
  // call says, the moves it reports, whether the listing changed, and the tree after; a
  // folder of a view removed or renamed takes what the build made of it in the mirror,
  // when the build reads the view, its folder in read-folders as a configure writes it,
  // and leaves it for a copy hosted from elsewhere, named like the view it copies.
  bench.check("Ports.cpp/the file port", [](Rows &rows) {
    struct Case {
      std::string_view said;
      std::function<void(FilePort &, const std::filesystem::path &)> call;
      bool unbuilds = false; // done, it reaches the mirror
    };
    const auto at = [](const std::filesystem::path &folder, std::string_view path) {
      return (folder / path).string();
    };
    const std::vector<Case> cases{
        {"remove a node's folder",
         [&](FilePort &port, const auto &folder) { port.remove(at(folder, "v/n")); }},
        {"remove a node's last file",
         [&](FilePort &port, const auto &folder) {
           port.remove(at(folder, "v/n/N.comp"));
         },
         true},
        {"remove the view's manifest",
         [&](FilePort &port, const auto &folder) {
           port.remove(at(folder, "v/view.vlp"));
         },
         true},
        {"rename a node's folder",
         [&](FilePort &port, const auto &folder) {
           port.rename(at(folder, "v/n"), at(folder, "v/m"));
         },
         true},
        {"rename into a hidden trash",
         [&](FilePort &port, const auto &folder) {
           port.make(at(folder, "v/.vulpen"));
           port.rename(at(folder, "v/n"), at(folder, "v/.vulpen/n"));
         },
         true},
        {"save a new file",
         [&](FilePort &port, const auto &folder) {
           port.save(std::string_view(at(folder, "v/n/new")), "x");
         }},
        {"save over a file",
         [&](FilePort &port, const auto &folder) {
           port.save(std::string_view(at(folder, "v/n/N.comp")), "x");
         }},
        {"save a hidden file",
         [&](FilePort &port, const auto &folder) {
           port.save(std::string_view(at(folder, "v/.kept")), "x");
         }},
        {"a relative path", [](FilePort &port, const auto &) { port.remove("v/n"); }},
        {"read a folder",
         [&](FilePort &port, const auto &folder) { port.read(at(folder, "v/n")); }},
        {"list a file",
         [&](FilePort &port, const auto &folder) {
           port.list(at(folder, "v/view.vlp"));
         }},
        {"list nothing",
         [&](FilePort &port, const auto &folder) { port.list(at(folder, "v/none")); }},
        {"copy a folder",
         [&](FilePort &port, const auto &folder) {
           port.copy(at(folder, "v/n"), at(folder, "v/c"));
         }},
    };
    const auto run = [&](const Case &each, bool read, std::string_view copy) {
      const std::filesystem::path folder = planted(rows,
                                                   {"v/",
                                                    "v/view.vlp=version 1",
                                                    "v/n/",
                                                    "v/n/N.comp=main",
                                                    "mirror/v/n/",
                                                    "mirror/v/n/N.comp.spv=built",
                                                    "mirror/v/x/"});
      // A line with no newline after it, which the tree shows whole on the row.
      if (read)
        Files::save(folder / VP_READ_FILE, folder.generic_string());
      const std::string ran = rows.quiet([&] {
        Rig rig(folder);
        const std::string result = said([&] { each.call(rig.file_port(), folder); });
        Names names;
        const std::vector<Moved> moved(rig.file_port().moved().begin(),
                                       rig.file_port().moved().end());
        return std::format("{}; moved {}; relisted {}",
                           result,
                           show(moved, names),
                           rig.ports.relisted());
      });
      rows.row(
          here(std::format("{}{} -> {}; then {}", each.said, copy, ran, tree_of(folder)),
               folder));
    };
    for (const Case &each : cases)
      run(each, true, "");
    for (const Case &each : cases)
      if (each.unbuilds)
        run(each, false, ", of a copy the build reads no folder of");
  });

  // With no window, copy and paste keep to the port, for a node reading the input whole
  // and for one outside a hold alike.
  bench.check("Ports.cpp/the clipboard, with no window", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::string said = rows.quiet([&] {
      Rig rig(folder);
      InputPort &input = rig.input();
      std::string text = std::format("1 windowed {}, the clipboard holds {}\n",
                                     input.windowed(),
                                     escaped(input.clipboard()));
      input.copy("copied");
      text +=
          std::format("2 a copy: the clipboard holds {}\n", escaped(input.clipboard()));
      rig.ports.hold_pointer("v", "menu.a");
      rig.ports.frame();
      InputPort &outside = rig.ports.input("v", "other", true);
      outside.copy("from outside");
      text += std::format("3 a node outside a hold: windowed {}, a copy, the clipboard "
                          "holds {}\n",
                          outside.windowed(),
                          escaped(input.clipboard()));
      return text;
    });
    for (const auto piece : std::views::split(said, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(std::string(line));
  });

  // Standard input as a file holds it, as a script piped into vulpen reaches it: each
  // frame's lines, read once a frame for every node that asks, a line's \r dropped, the
  // last line, with no break, once the input ends; and what prints: a line, given a break
  // where it has none, and the line typed drawn around its caret in as much as a row
  // holds, a pipe's row being 80 columns. No terminal, so nothing prompts; keys need one.
  bench.check("Ports.cpp/the terminal, read from a file", [](Rows &rows) {
    const std::filesystem::path typed = rows.root() / "typed";
    Files::save(typed, "a\nb\r\n\nc");
    if (std::freopen(typed.string().c_str(), "rb", stdin) == nullptr) {
      rows.row("standard input cannot be read from a file here");
      return;
    }
    // Made and let go quietly, as its log prints a header and a footer.
    std::optional<Rig> rig;
    rows.quiet([&] { return &rig.emplace(rows.root()); });
    TerminalPort &port = rig->ports;
    const auto lines = [](std::span<const std::string> read) {
      std::string text;
      for (const std::string &line : read)
        text += (text.empty() ? "" : ", ") + escaped(line);
      return "[" + text + "]";
    };
    for (int frame = 1; frame <= 3; ++frame) {
      rig->ports.frame();
      const std::string first = lines(port.lines());
      rows.row(std::format("frame {}: reads {}, asked again {}, ended {}",
                           frame,
                           first,
                           lines(port.lines()),
                           port.ended()));
    }
    rows.row(std::format("typed {}, {} columns", port.typed(), port.columns()));
    port.prompt("> ");
    rows.row("a prompt");
    port.print("a line");
    rows.row("a line printed");
    port.print("a line\n");
    rows.row("a line with its break printed");
    port.line("abc", 1);
    rows.row("abc typed, the caret after a");
    const std::string long_line(100, 'x');
    port.line(long_line, 100);
    rows.row("100 typed, the caret at the end");
    port.line(long_line, 79);
    rows.row("100 typed, the caret at 79");
    port.line(long_line, 80);
    rows.row("100 typed, the caret at 80");
    port.line("", 0);
    rows.row("nothing typed");
    rows.quiet([&] {
      rig.reset();
      return 0;
    });
  });
  bench.leaves_out("Ports.cpp/the terminal, read from a file",
                   "threads",
                   "standard input is the process's one, which two at once would each "
                   "take for their own file");

  bench.check("Ports.cpp/view_of", [](Rows &rows) {
    const std::filesystem::path folder =
        planted(rows, {"v/", "v/view.vlp=version 1", "v/n/", "v/n/m/", "w/"});
    for (const std::string_view path :
         {"v", "v/n", "v/n/m", "v/n/m/f", "w", "v/view.vlp", ""})
      rows.row(here(
          std::format("{} -> {}", path, escaped(view_of(folder / path).generic_string())),
          folder));
  });

  for (const char *const id :
       {"Ports.cpp/the file port", "Ports.cpp/files the nodes open"})
    bench.leaves_out(id,
                     "locale",
                     "the messages carry the C library's, in the process's "
                     "language");
}

} // namespace VP::bench
