#include "baseclasses/Platform.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"
#include "tools/bench/Worlds.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// baseclasses/Platform.cpp, its files (9i): each row plants a tree of files in a folder
// of its own in the sandbox, runs one call on paths in it, as the file port passes them,
// absolute, and shows what the call said and the tree after, the folder as <here>. A
// tree's "path=text" is a file, "path/" a folder. bench.py plants <root>/fixtures/loop/,
// a folder holding a file and up, a link to the folder above, as no bench file makes a
// link (RA04).

namespace VP::bench {

// The window's own GLFW handle, so a row reads back what a call set on it: its title,
// its monitor, its cursor's mode.
struct WindowHandle {
  using type = GLFWwindow *Window::*;
  friend constexpr type reach(WindowHandle);
};
template struct Reach<WindowHandle, &Window::_window>;

namespace domain {
// A character as GLFW hands it on: a Unicode code point, and what a broken input method
// may hand on past them.
struct CodePoint {};
// A character's first byte: each of UTF-8's kinds at both its ends, and the bytes that
// start none.
struct FirstByte {};
} // namespace domain

template <> struct Gen<domain::FirstByte> {
  using type = unsigned char;
  static std::vector<unsigned char> edges() {
    return {0x41,
            0x00,
            0x7f,
            0x80,
            0xbf,
            0xc0,
            0xc1,
            0xc2,
            0xdf,
            0xe0,
            0xef,
            0xf0,
            0xf4,
            0xf5,
            0xf7,
            0xf8,
            0xfe,
            0xff};
  }
  static unsigned char random(Random &random) {
    return static_cast<unsigned char>(random.bits());
  }
};

template <> struct Gen<domain::CodePoint> {
  using type = std::uint32_t;
  static std::vector<std::uint32_t> edges() {
    return {0x41,
            0,
            0x7f,
            0x80,
            0x7ff,
            0x800,
            0xd800,
            0xffff,
            0x10000,
            0x20ac,
            0x1f600,
            0x10ffff,
            0x110000,
            0x7fffffff,
            0xffffffff};
  }
  static std::uint32_t random(Random &random) {
    return static_cast<std::uint32_t>(random.below(0x110000));
  }
};

// "done", or what the call threw.
template <class F> std::string said(F &&call) {
  try {
    call();
    return "done";
  } catch (...) {
    return thrown();
  }
}

void cases(Bench &bench) {
  bench.sweep(
      "Platform.cpp/Files::save",
      [](const Rows &rows,
         const std::vector<std::string> &tree,
         std::string_view file,
         std::string_view text) {
        const std::filesystem::path folder = planted(rows, tree);
        return after(folder, [&] { Files::save(folder / file, text); });
      },
      domain::Tree{},
      domain::TreePath{},
      domain::Word{});

  // A copy goes whole or not at all, to a path where nothing is yet; a temp a killed copy
  // left goes first, as its comment says.
  bench.sweep(
      "Platform.cpp/Files::copy",
      [](const Rows &rows,
         const std::vector<std::string> &tree,
         std::string_view from,
         std::string_view to) {
        const std::filesystem::path folder = planted(rows, tree);
        return after(folder, [&] { Files::copy(folder / from, folder / to); });
      },
      domain::Tree{},
      domain::TreePath{},
      domain::TreePath{});

  bench.sweep(
      "Platform.cpp/Files::rename",
      [](const Rows &rows,
         const std::vector<std::string> &tree,
         std::string_view from,
         std::string_view to) {
        const std::filesystem::path folder = planted(rows, tree);
        return after(folder, [&] { Files::rename(folder / from, folder / to); });
      },
      domain::Tree{},
      domain::TreePath{},
      domain::TreePath{});

  bench.sweep(
      "Platform.cpp/Files::remove",
      [](const Rows &rows, const std::vector<std::string> &tree, std::string_view file) {
        const std::filesystem::path folder = planted(rows, tree);
        return after(folder, [&] { Files::remove(folder / file); });
      },
      domain::Tree{},
      domain::TreePath{});

  bench.sweep(
      "Platform.cpp/Files::make",
      [](const Rows &rows, const std::vector<std::string> &tree, std::string_view made) {
        const std::filesystem::path folder = planted(rows, tree);
        return after(folder, [&] { Files::make(folder / made); });
      },
      domain::Tree{},
      domain::TreePath{});

  bench.sweep(
      "Platform.cpp/Files::text",
      [](const Rows &rows, const std::vector<std::string> &tree, std::string_view file) {
        const std::filesystem::path folder = planted(rows, tree);
        std::string read;
        return after(folder, [&] { read = Files::text(folder / file); }) + "; read " +
               escaped(read);
      },
      domain::Tree{},
      domain::TreePath{});

  bench.holds("Platform.cpp/Files::save",
              "a save then a read gives the text back",
              [](const Rows &rows, std::string_view text) {
                const std::filesystem::path folder = planted(rows, {});
                Files::save(folder / "f", text);
                return Files::text(folder / "f") == text;
              });

  // A file or a folder of a person's that is named as a save's or a copy's temp: what
  // each call leaves of it (9i).
  bench.check("Platform.cpp/Files beside a person's .tmp", [](Rows &rows) {
    struct Case {
      std::string_view said;
      std::vector<std::string> tree;
      std::function<void(const std::filesystem::path &)> call;
    };
    const std::vector<Case> cases{
        {"save a beside a.tmp, a file",
         {"a=1", "a.tmp=mine"},
         [](const std::filesystem::path &folder) { Files::save(folder / "a", "2"); }},
        {"save a beside a.tmp/, a folder",
         {"a=1", "a.tmp/", "a.tmp/keep=mine"},
         [](const std::filesystem::path &folder) { Files::save(folder / "a", "2"); }},
        {"copy a to x beside x.tmp, a file",
         {"a=1", "x.tmp=mine"},
         [](const std::filesystem::path &folder) {
           Files::copy(folder / "a", folder / "x");
         }},
        {"copy a to x beside x.tmp/, a folder",
         {"a=1", "x.tmp/", "x.tmp/keep=mine"},
         [](const std::filesystem::path &folder) {
           Files::copy(folder / "a", folder / "x");
         }},
        {"copy d/ to x beside x.tmp/, a folder",
         {"d/", "d/b=2", "x.tmp/", "x.tmp/keep=mine"},
         [](const std::filesystem::path &folder) {
           Files::copy(folder / "d", folder / "x");
         }},
    };
    for (const Case &each : cases) {
      const std::filesystem::path folder = planted(rows, each.tree);
      rows.row(
          std::format("{} -> {}", each.said, after(folder, [&] { each.call(folder); })));
    }
  });

  // A folder holding a link to the folder above: the copy follows it into itself.
  bench.check("Platform.cpp/Files::copy a link to the folder above", [](Rows &rows) {
    // In the root, or above it, as two threads at once each get a folder of the root.
    std::filesystem::path loop = rows.root() / "fixtures" / "loop";
    if (!std::filesystem::exists(loop))
      loop = rows.root().parent_path() / "fixtures" / "loop";
    if (!std::filesystem::exists(loop)) {
      rows.row("no fixture: bench.py plants <root>/fixtures/loop/");
      return;
    }
    const std::filesystem::path folder = planted(rows, {});
    rows.row("copy fixtures/loop to <here>/copy -> " +
             after(folder, [&] { Files::copy(loop, folder / "copy"); }));
  });

  // Every file a folder holds goes, at any depth, its folders kept; a link is not
  // followed.
  bench.sweep(
      "Platform.cpp/Files::clear",
      [](const Rows &rows,
         const std::vector<std::string> &tree,
         std::string_view folder) {
        const std::filesystem::path planted_at = planted(rows, tree);
        return after(planted_at, [&] { Files::clear(planted_at / folder); });
      },
      domain::Tree{},
      domain::TreePath{});

  bench.sweep("Platform.cpp/key_named", key_named, domain::Word{});
  // The text a window hands on for a character typed, and how long a character is by its
  // first byte, as a key's name is checked against it.
  bench.sweep("Platform.cpp/utf8", utf8, domain::CodePoint{});
  bench.sweep("Platform.cpp/utf8_length", utf8_length, domain::FirstByte{});
  // Whether a library is mapped already: the C library always is, a file of none never.
  bench.check("Platform.cpp/Library::mapped", [](Rows &rows) {
    rows.row(std::format("libc.so.6 -> {}", Library::mapped("libc.so.6")));
    rows.row(std::format("a library no file holds -> {}",
                         Library::mapped(rows.root() / "none.so")));
  });
  bench.holds(
      "Platform.cpp/utf8",
      "a character's first byte gives its length",
      [](std::uint32_t point) {
        const std::string text = utf8(point);
        return utf8_length(static_cast<unsigned char>(text.front())) == text.size();
      },
      domain::CodePoint{});

  // The terminal as a run sees it: whether it shows colors and how wide a row is follow
  // TERM, NO_COLOR and COLUMNS, as bench.py's environment context sets them.
  bench.check("Platform.cpp/Terminal", [](Rows &rows) {
    rows.row(std::format(
        "colors -> {}; columns -> {}", Terminal::colors(), Terminal::columns()));
  });

  // A gamepad at rest, as none present reads too: no button down, its sticks centered
  // and its triggers let go, which GLFW reads as -1.
  bench.check("Platform.cpp/resting", [](Rows &rows) {
    const GLFWgamepadstate state = resting();
    std::string pressed;
    for (const unsigned char button : state.buttons)
      pressed += std::to_string(button);
    std::string axes;
    for (const float axis : state.axes)
      axes += std::format(" {}", axis);
    rows.row(std::format("buttons {}; axes{}", pressed, axes));
  });
  // The names a gamepad's sticks and triggers go by, and no other: a button's is a key's.
  bench.check("Platform.cpp/axis_named", [](Rows &rows) {
    for (const std::string_view name : {"gamepad_left",
                                        "gamepad_right",
                                        "gamepad_left_trigger",
                                        "gamepad_right_trigger",
                                        "gamepad_a",
                                        "gamepad_left_x",
                                        "gamepad",
                                        "left",
                                        ""})
      rows.row(std::format(
          "{} -> axis {}, key {}", escaped(name), axis_named(name), key_named(name)));
  });
  // What sokol_audio's logger keeps for trouble(): a problem's message, or its number
  // where the build keeps no messages; a line past the problems, none. No case opens
  // the speakers, as sokol_audio would reach the desktop's sound server, or start one:
  // Speakers' device paths are read.
  bench.check("Platform.cpp/on_sokol", [](Rows &rows) {
    SpeakersTrouble trouble;
    const auto kept = [&] {
      const char *const message = trouble.message.load();
      return std::format("message {}, item {}, fresh {}",
                         message ? escaped(message) : std::string("none"),
                         trouble.item.load(),
                         trouble.fresh.load());
    };
    on_sokol(
        "saudio", sokol_information, 7, "a line past the problems", 0, nullptr, &trouble);
    rows.row("information -> " + kept());
    on_sokol("saudio", 1, 9, "the device went away", 0, nullptr, &trouble);
    rows.row("an error -> " + kept());
    trouble.fresh.store(false);
    on_sokol("saudio", 2, 11, nullptr, 0, nullptr, &trouble);
    rows.row("a warning of no message -> " + kept());
    rows.row(std::format("this build hears sound -> {}", Speakers::heard()));
  });

  // A window's title, filling the monitor and back, the pointer held and let go, and a
  // poll with no gamepad present, each read back from GLFW's own handle.
  bench.check("Platform.cpp/Window, its desktop calls", [](Rows &rows) {
    on_window(rows, [&](Gpu &, const Window &window) {
      GLFWwindow *const handle = window.*reach(WindowHandle{});
      window.title("a title, é");
      rows.row("title -> " + escaped(glfwGetWindowTitle(handle)));
      const auto placed = [&] {
        return std::format("on a monitor {}, size {}x{}",
                           glfwGetWindowMonitor(handle) != nullptr,
                           window.size().width,
                           window.size().height);
      };
      // Each call made before what it changed is read back, as a format's arguments are
      // evaluated in no set order.
      for (const bool fills : {true, true, false, false}) {
        const std::string done = said([&] { window.fullscreen(fills); });
        rows.row(std::format("fullscreen {} -> {}; {}", fills, done, placed()));
      }
      for (const bool locked : {true, false}) {
        window.lock_pointer(locked);
        const int mode = glfwGetInputMode(handle, GLFW_CURSOR);
        rows.row(std::format("lock_pointer {} -> cursor {}, raw motion {} (offered {})",
                             locked,
                             mode == GLFW_CURSOR_DISABLED ? std::string("disabled")
                             : mode == GLFW_CURSOR_NORMAL ? std::string("normal")
                                                          : std::to_string(mode),
                             glfwGetInputMode(handle, GLFW_RAW_MOUSE_MOTION),
                             glfwRawMouseMotionSupported()));
      }
      struct Counted final : WindowInput {
        std::vector<std::string> said;
        void key(std::string_view name, bool down) override {
          said.push_back(std::format("key {} {}", name, down));
        }
        void text(std::string_view) override {}
        void pointer(float, float) override {}
        void button(std::string_view, bool) override {}
        void wheel(float, float) override {}
        void focus(bool) override {}
        void axis(std::string_view name, float x, float y) override {
          said.push_back(std::format("axis {} {} {}", name, x, y));
        }
      } input;
      // poll hands on the gamepad's changes after the window's events (poll_gamepad).
      window.poll(input);
      window.poll(input);
      rows.row(std::format("two polls, no gamepad present -> {} keys or axes handed on",
                           input.said.size()));
    });
  });
  takes_the_main_thread(bench, "Platform.cpp/Window, its desktop calls");

  // A window on Xvfb's display, which no person sees: its size as made, the clipboard
  // through it, and a poll with nothing sent.
  bench.check("Platform.cpp/Window", [](Rows &rows) {
    on_window(rows, [&](Gpu &, const Window &window) {
      rows.row(std::format(
          "made 640x480 -> size {}x{}", window.size().width, window.size().height));
      window.copy("copied é");
      rows.row("copy, then clipboard -> " + escaped(window.clipboard()));
      struct Quiet final : WindowInput {
        std::size_t events = 0;
        void key(std::string_view, bool) override {
          ++events;
        }
        void text(std::string_view) override {
          ++events;
        }
        void pointer(float, float) override {
          ++events;
        }
        void button(std::string_view, bool) override {
          ++events;
        }
        void wheel(float, float) override {
          ++events;
        }
        void focus(bool) override {
          ++events;
        }
        void axis(std::string_view, float, float) override {
          ++events;
        }
      } input;
      const bool open = window.poll(input);
      rows.row(std::format("poll with nothing sent -> open {}", open));
    });
  });
  takes_the_main_thread(bench, "Platform.cpp/Window");
  bench.leaves_out(
      "Platform.cpp/Terminal",
      "environment",
      "colors and columns follow TERM, NO_COLOR and COLUMNS, as they should");

  for (const char *const id : {"Platform.cpp/Files::save",
                               "Platform.cpp/Files::copy",
                               "Platform.cpp/Files::rename",
                               "Platform.cpp/Files::remove",
                               "Platform.cpp/Files::make",
                               "Platform.cpp/Files::text",
                               "Platform.cpp/Files::copy a link to the folder above",
                               "Platform.cpp/Files::clear"})
    bench.leaves_out(id,
                     "locale",
                     "the messages carry the C library's, in the process's "
                     "language");
}

} // namespace VP::bench
