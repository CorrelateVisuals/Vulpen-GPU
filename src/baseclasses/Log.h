#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace VP {

// How much a run says. Each level adds to the one before it, and warn is the default,
// so a run that goes as meant is quiet (C09):
// - error: what failed, naming its cause (A02);
// - warn: what runs, but not as meant;
// - info: each step of the run, once: the view, the GPU, the window, each node's
//   pipeline, operator and buffers, each live swap, and how the run ended;
// - debug: what a bug hunt needs besides: each GPU weighed, each node's pass block, what
//   a swap keeps, each remade swapchain. The engine never logs a frame, so debug stays
//   readable; an operator may, at its node's level (V09).
enum class Level { error, warn, info, debug };

inline constexpr std::array level_names{std::string_view{"error"},
                                        std::string_view{"warn"},
                                        std::string_view{"info"},
                                        std::string_view{"debug"}};

inline std::optional<Level> level_named(std::string_view name) {
  for (std::size_t index = 0; index < level_names.size(); ++index)
    if (level_names[index] == name)
      return static_cast<Level>(index);
  return std::nullopt;
}

// What a line is about, as its {tag} shows; on a terminal each tag has one color, and
// tags about the same thing share it:
// - run: the run itself, from the build it runs to how it ended;
// - gpu, swp: the machine: the GPU, and the swapchain of the window;
// - nod, mem: the graph: what the engine makes for a node, and the buffers it writes;
// - mod: the nodes' modules and their live builds;
// - out: what a node's operator writes.
enum class Tag { run, gpu, swp, nod, mem, mod, out };

// The bottom of the tree: every module logs, so this header includes nothing of ours.
// A header and a footer frame the log. Each line between them goes to standard output
// under the time it was written, which shows only when the second changes:
//
//    26.09.30 10:40:12 {run} vulpen 6214db8
//                      {nod} wave: pipeline from Wave.comp
//                      {!!!} view.vlp node probe: param every = x is not a uint
//
// An error shows {!!!} and a warning { ! } in place of its tag, so a problem stands out
// in a file as well as in color. A line that repeats prints once, then a count.
class Log {
public:
  // title: what this binary is, which ends the header at every level (RC07).
  Log(Level level, std::string_view title);
  // Prints the footer.
  ~Log();
  Log(const Log &) = delete;
  Log &operator=(const Log &) = delete;

  Level level() const;
  void write(Level level, Tag tag, std::string_view text) const;
  // A line about a node: its own level wins over the process level (V09), and its name
  // leads the text.
  void write(Level level,
             Level shown,
             Tag tag,
             std::string_view node,
             std::string_view text) const;

private:
  void print(Level level, Tag tag, std::string_view node, std::string_view text) const;
  void print_repeats() const;
  void emit(std::string_view tag, const char *color, std::string_view text) const;

  const Level _level;
  const bool _colors;
  // What a write changes is only what the log prints next, so writers hold it const.
  // The two lines keep their capacity, so a line an operator logs each frame allocates
  // nothing (CPP10).
  mutable std::string _line;
  mutable std::string _previous;
  mutable std::uint64_t _repeats = 0;
  mutable std::time_t _second = 0;
};

} // namespace VP
