#include "baseclasses/Log.cpp"

#include "runtime/Text.h"
#include "tools/bench/Bench.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// baseclasses/Log.h and Log.cpp: the log every module writes to (C09, V09). A row's log
// keeps its file in the row's sandbox, and the rows show what it printed as that file
// holds it, plain, from past its header, without the clock's column; what went to
// standard output besides is dropped, unless a row is about it.

namespace VP::bench {

struct LogKept {
  using type = bool (Log::*)(Level, std::string_view) const;
  friend constexpr type reach(LogKept);
};
template struct Reach<LogKept, &Log::kept>;
struct LogAddToFile {
  using type = void (Log::*)() const;
  friend constexpr type reach(LogAddToFile);
};
template struct Reach<LogAddToFile, &Log::add_to_file>;
struct LogText {
  using type = std::string Log::*;
  friend constexpr type reach(LogText);
};
template struct Reach<LogText, &Log::_text>;
struct LogOut {
  using type = std::string Log::*;
  friend constexpr type reach(LogOut);
};
template struct Reach<LogOut, &Log::_out>;
struct LogStart {
  using type = std::size_t Log::*;
  friend constexpr type reach(LogStart);
};
template struct Reach<LogStart, &Log::_start>;
struct LogUnsaved {
  using type = bool Log::*;
  friend constexpr type reach(LogUnsaved);
};
template struct Reach<LogUnsaved, &Log::_unsaved>;
struct LogPrinted {
  using type = void (Log::*)(std::string_view, bool) const;
  friend constexpr type reach(LogPrinted);
};
template struct Reach<LogPrinted, &Log::printed>;

namespace domain {
// What a line of the log says: one line or several, ends of lines, bytes a terminal
// obeys.
struct LogText {};
// A view a line is about, by the name a line addresses it with.
struct ViewName {};
// The views keep_to keeps to.
struct ViewNames {};
// add_to_file's sizes, about the file's room: a header's bytes, rows of lines, and how
// long the line added is.
struct HeaderBytes {};
struct LineRows {};
struct Added {};
} // namespace domain

template <> struct Gen<domain::LogText> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"a",
            "",
            "a\nb",
            "a\n",
            "a\n\n",
            "\n",
            "\x1b[2J",
            "a\tb",
            "\x7f",
            "a\r",
            "a\r\nb",
            "{!!!} a",
            "\xc3\xa9",
            "%s %n",
            std::string(300, 'a')};
  }
  static std::string random(Random &random) {
    return words::drawn(random, "ab \n\t\r\x1b", 10);
  }
};

template <> struct Gen<domain::ViewName> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"", "a", "b", "ide"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::ViewNames> {
  using type = std::vector<std::string>;
  static std::vector<type> edges() {
    return {{"a"}, {}, {"a", "b"}, {"ide"}, {""}};
  }
  static type random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

// Sizes from the edges' alone: the file's room is 1 MiB, which 210 rows of 50 lines of
// 100 bytes pass.
template <class Domain, std::size_t... Values> struct Sizes {
  using type = std::size_t;
  static std::vector<std::size_t> edges() {
    return {Values...};
  }
  static std::size_t random(Random &random) {
    return edges()[random.below(sizeof...(Values))];
  }
};
template <> struct Gen<domain::HeaderBytes> : Sizes<domain::HeaderBytes, 10, 0, 63> {};
template <>
struct Gen<domain::LineRows> : Sizes<domain::LineRows, 1, 0, 209, 210, 211, 299> {};
template <> struct Gen<domain::Added> : Sizes<domain::Added, 1, 0, 2> {};

template <> struct Gen<Level> {
  using type = Level;
  static std::vector<Level> edges() {
    return {Level::warn, Level::error, Level::info, Level::debug};
  }
  static Level random(Random &random) {
    return static_cast<Level>(random.below(level_names.size()));
  }
};

template <> struct Gen<Tag> {
  using type = Tag;
  static std::vector<Tag> edges() {
    return {Tag::run, Tag::gpu, Tag::swp, Tag::nod, Tag::mem, Tag::mod, Tag::out};
  }
  static Tag random(Random &random) {
    return static_cast<Tag>(random.below(marks.size()));
  }
};

template <> struct Shown<Level> {
  static std::string text(Level level, Names &) {
    return std::string(level_names.at(static_cast<std::size_t>(level)));
  }
};

template <> struct Shown<Tag> {
  static std::string text(Tag tag, Names &) {
    return std::string(marks.at(static_cast<std::size_t>(tag)).text);
  }
};

template <> struct Shown<Logged> {
  static std::string text(const Logged &line, Names &names) {
    return show(line.level, names) + " " + escaped(line.text);
  }
};

namespace {

constexpr std::string_view title = "bench";
constexpr std::string_view file_name = "run.log";
constexpr std::size_t big_line = 100;
// The add_to_file rows: rows of this many lines of big_line bytes.
constexpr std::size_t rows_of_lines = 50;

// A log's lines without the clock's column, the header's and footer's by a word each.
std::vector<std::string> unclocked(std::string_view text) {
  std::vector<std::string> lines;
  for (const std::string_view line : lines_in(text)) {
    const std::string_view shown =
        line.substr(std::min(line.size(), std::size_t{time_width}));
    lines.emplace_back(shown == rule     ? "<rule>"
                       : shown == banner ? "<banner>"
                       : shown == credit ? "<credit>"
                                         : std::string(shown));
  }
  return lines;
}

// What the log printed past its header, as its file holds it, each line without the
// clock's column.
std::vector<std::string> written(const Log &log) {
  return unclocked(
      std::string_view(log.*reach(LogText{})).substr(log.*reach(LogStart{})));
}

std::vector<Logged> taken(const Log &log) {
  std::vector<Logged> lines;
  lines.resize(log.take(lines));
  return lines;
}

std::filesystem::path fresh(const Rows &rows) {
  const std::filesystem::path file = rows.root() / file_name;
  if (std::filesystem::exists(file))
    Files::remove(file);
  return file;
}

// What runs before left in a file of this many bytes: numbered lines of big_line bytes,
// the first one longer, so the whole comes out even.
std::string runs_before(std::size_t size) {
  const std::size_t after_first = size / big_line - 1;
  std::string text;
  for (std::size_t number = 0; number <= after_first; ++number) {
    std::string line = std::format("before {:05}", number);
    line.resize((number == 0 ? size - after_first * big_line : big_line) - 1, '.');
    text.append(line).append("\n");
  }
  return text;
}

// Whether a file's first line starts under the date and time, not the blank column.
bool timed(std::string_view text) {
  return text.size() >= time_width &&
         text.substr(0, time_width) != std::string(time_width, ' ');
}

// A log, written to, as a row records it: what it printed and what take hands on.
struct Said {
  std::vector<std::string> printed;
  std::vector<Logged> kept;
};

} // namespace

template <> struct Shown<Said> {
  static std::string text(const Said &said, Names &names) {
    return "printed " + show(said.printed, names) + " kept " + show(said.kept, names);
  }
};

void cases(Bench &bench) {
  bench.sweep("Log.h/level_named", level_named, domain::Word{});
  bench.holds("Log.h/level_named", "each level's name reads back as it", [](Level level) {
    return level_named(level_names.at(static_cast<std::size_t>(level))) == level;
  });

  // Made with a file that holds a run before, or none, and gone again: whether the file
  // still starts with the run before, and its lines after it.
  bench.sweep(
      "Log.cpp/Log::Log",
      [](const Rows &rows, Level level, std::string_view before) {
        const std::filesystem::path file = fresh(rows);
        if (!before.empty())
          Files::save(file, before);
        rows.quiet([&] { return Log(level, title, file.string()).level(); });
        const std::string text = Files::text(file);
        return std::pair{text.starts_with(before),
                         unclocked(std::string_view(text).substr(before.size()))};
      },
      domain::Type{},
      domain::LogText{});
  bench.sweep("Log.cpp/Log::Log a folder for a file", [](const Rows &rows, Level level) {
    const std::filesystem::path folder = rows.root() / "folder.log";
    if (!std::filesystem::exists(folder / "inside"))
      Files::save(folder / "inside", "");
    return rows.quiet([&] { return written(Log(level, title, folder.string())); });
  });
  bench.leaves_out("Log.cpp/Log::Log a folder for a file",
                   "locale",
                   "the message is the C library's, in the process's language");

  // The runs before keep at most half the file's room, 524,288 bytes: their newest lines,
  // each whole, and all of them at exactly half. A row shows the lines kept, by number.
  bench.check("Log.cpp/Log::Log runs before, past half the room", [](Rows &rows) {
    for (const std::size_t past : {0, 1, 1000}) {
      const std::filesystem::path file = fresh(rows);
      const std::size_t size = file_capacity / 2 + past;
      Files::save(file, runs_before(size));
      rows.quiet([&] { return Log(Level::warn, title, file.string()).level(); });
      const std::string text = Files::text(file);
      std::size_t kept = 0;
      std::string_view first;
      std::string_view last;
      for (const std::string_view line : lines_in(text)) {
        if (line.starts_with(' ')) // this run's lines start in the clock's column
          continue;
        last = line.substr(0, line.find('.'));
        if (kept++ == 0)
          first = last;
      }
      rows.row(std::format(
          "{} bytes before -> {} lines kept, \"{}\" to \"{}\"", size, kept, first, last));
    }
  });

  bench.sweep(
      "Log.cpp/Log::write",
      [](const Rows &rows, Level process, Level level, Tag tag, std::string_view text) {
        return rows.quiet([&] {
          const Log log(process, title, fresh(rows).string());
          taken(log);
          log.write(level, tag, text);
          return Said{written(log), taken(log)};
        });
      },
      domain::Type{},
      domain::Type{},
      domain::Type{},
      domain::LogText{});

  bench.sweep(
      "Log.cpp/Log::write about a node",
      [](const Rows &rows,
         Level level,
         Level shown,
         std::string_view view,
         std::string_view node,
         std::string_view text) {
        return rows.quiet([&] {
          const Log log(Level::warn, title, fresh(rows).string());
          const std::vector<std::string> kept{"a"};
          log.keep_to(kept);
          taken(log);
          log.write(level, shown, Tag::nod, view, node, text);
          return Said{written(log), taken(log)};
        });
      },
      domain::Type{},
      domain::Type{},
      domain::ViewName{},
      domain::Name{},
      domain::LogText{});

  bench.sweep(
      "Log.cpp/Log::kept",
      [](const Rows &rows,
         const std::vector<std::string> &views,
         Level level,
         std::string_view view) {
        return rows.quiet([&] {
          const Log log(Level::warn, title);
          log.keep_to(views);
          return (log.*reach(LogKept{}))(level, view);
        });
      },
      domain::ViewNames{},
      domain::Type{},
      domain::ViewName{});

  // The same line again only counts, until another line or apart.
  bench.sweep(
      "Log.cpp/Log::print repeats",
      [](const Rows &rows, std::uint8_t times, bool apart, std::string_view view) {
        return rows.quiet([&] {
          const Log log(Level::debug, title, fresh(rows).string());
          for (std::uint8_t time = 0; time < times % 5; ++time) {
            log.write(Level::info, Level::info, Tag::nod, view, "a", "same");
            if (apart)
              log.apart();
          }
          log.write(Level::info, Tag::run, "other");
          return Said{written(log), taken(log)};
        });
      },
      domain::Type{},
      domain::Type{},
      domain::ViewName{});

  // What a node printed: to std::cout at the warn level unless the node's level is
  // error, to std::cerr as an error, a line at a time; a line not ended prints as the
  // Printed goes, after the speaker went, as the log's owner holds it for the run.
  bench.sweep(
      "Log.cpp/Printed",
      [](const Rows &rows, std::string_view out, std::string_view error, Level shown) {
        return rows.quiet([&] {
          const Log log(Level::warn, title, fresh(rows).string());
          {
            const Printed printed(log);
            const Log::Speaking speaking(log, {.view = "", .node = "n", .shown = shown});
            std::cout << out;
            std::cerr << error;
          }
          return Said{written(log), taken(log)};
        });
      },
      domain::LogText{},
      domain::LogText{});
  bench.leaves_out("Log.cpp/Printed",
                   "threads",
                   "std::cout and std::cerr are the process's, which one Printed holds");

  // A string reaches the stream a run at a time; put and std::endl hand it a character
  // at a time, which reaches the log alike.
  bench.check("Log.cpp/Printed, a character at a time", [](Rows &rows) {
    const Said said = rows.quiet([&] {
      const Log log(Level::warn, title, fresh(rows).string());
      taken(log);
      {
        const Printed printed(log);
        const Log::Speaking speaking(log, {.node = "n", .shown = Level::warn});
        std::cout << "ab" << std::endl;
        std::cout.put('c').put('\n');
        std::cerr.put('e') << std::endl;
      }
      return Said{written(log), taken(log)};
    });
    Names names;
    rows.row("\"ab\" and std::endl, put 'c' and '\\n', to errors put 'e' and std::endl "
             "-> " +
             show(said, names));
  });
  bench.leaves_out("Log.cpp/Printed, a character at a time",
                   "threads",
                   "std::cout and std::cerr are the process's, which one Printed holds");

  bench.holds("Log.cpp/Log::Speaking",
              "the speaker before comes back, however deep",
              [](const Rows &rows, std::uint8_t depth) {
                return rows.quiet([&] {
                  const Log log(Level::warn, title);
                  const auto speaker = [&] {
                    std::vector<Logged> lines;
                    log.apart();
                    (log.*reach(LogPrinted{}))("x", false);
                    lines.resize(log.take(lines));
                    return lines.empty() ? std::string("none") : lines.back().text;
                  };
                  const std::string before = speaker();
                  const std::function<void(int)> nest = [&](int left) {
                    if (left <= 0)
                      return;
                    const Log::Speaking speaking(log,
                                                 {.node = "n", .shown = Level::debug});
                    nest(left - 1);
                  };
                  nest(depth % 8);
                  return speaker() == before;
                });
              });

  // While a line is typed, each line the log prints first clears the row it takes: the
  // row shows what the call printed to standard output, the clear's bytes included.
  bench.check("Log.cpp/Log::clears_row", [](Rows &rows) {
    for (const bool clears : {false, true}) {
      const Log log(Level::warn, title);
      rows.skip();
      log.clears_row(clears);
      log.write(Level::warn, Tag::run, "a");
      rows.row(std::format("({}) then a warning", clears));
    }
    rows.skip();
  });

  // A file the log can no longer save is said once, and the run goes on.
  bench.check("Log.cpp/Log::save", [](Rows &rows) {
    const Said said = rows.quiet([&] {
      const std::filesystem::path file = fresh(rows);
      const Log log(Level::warn, title, file.string());
      Files::remove(file);
      Files::save(file / "inside", ""); // a folder where the file was
      taken(log);
      log.write(Level::error, Tag::run, "first");
      log.write(Level::error, Tag::run, "second");
      return Said{written(log), taken(log)};
    });
    Names names;
    rows.row("a folder where its file was, then two errors -> " + show(said, names));
    Files::remove(rows.root() / file_name / "inside");
  });
  bench.leaves_out("Log.cpp/Log::save",
                   "locale",
                   "the message is the C library's, in the process's language");

  // Saved when made, at each problem and at the end: an info line waits on disk for the
  // next save, and a warning takes it along. The first line the log prints is under the
  // date and time, as no second came before it.
  bench.check("Log.cpp/Log::save as the run goes", [](Rows &rows) {
    const std::filesystem::path file = fresh(rows);
    const auto on_disk = [&] {
      Names names;
      return show(unclocked(Files::text(file)), names);
    };
    {
      const Log log(Level::info, title, file.string());
      rows.skip();
      rows.row(std::format("1. made -> under the time {}, the file holds {}",
                           timed(Files::text(file)),
                           on_disk()));
      log.write(Level::info, Tag::run, "a");
      rows.skip();
      rows.row("2. an info line -> the file holds " + on_disk());
      log.write(Level::warn, Tag::run, "w");
      rows.skip();
      rows.row("3. a warning -> the file holds " + on_disk());
    }
    rows.skip();
  });

  // Past the file's room, the oldest half of the run's lines go, the header stays. The
  // log's own text is set by hand, and its save at the end held back.
  bench.sweep(
      "Log.cpp/Log::add_to_file",
      [](const Rows &rows, std::size_t header, std::size_t lines, std::size_t added) {
        return rows.quiet([&] {
          Log log(Level::warn, title, fresh(rows).string());
          std::string &text = log.*reach(LogText{});
          text.assign(header, 'h');
          for (std::size_t line = 0; line < lines * rows_of_lines; ++line)
            text.append(big_line - 1, 'a').append("\n");
          log.*reach(LogStart{}) = header;
          const std::string last = std::string(added * big_line, 'n') + "\n";
          log.*reach(LogOut{}) = last;
          log.*reach(LogUnsaved{}) = true;
          (log.*reach(LogAddToFile{}))();
          return std::format("{} bytes, header kept {}, last line kept {}",
                             text.size(),
                             text.substr(0, header) == std::string(header, 'h'),
                             text.ends_with(last));
        });
      },
      domain::HeaderBytes{},
      domain::LineRows{},
      domain::Added{});

  // At its room exactly the file keeps every line; a byte past it, the older half of the
  // run's lines go, as in the rows above.
  bench.check("Log.cpp/Log::add_to_file at its room", [](Rows &rows) {
    for (const std::size_t past : {0, 1}) {
      const std::string said = rows.quiet([&] {
        constexpr std::size_t header = 10;
        constexpr std::size_t lines = (file_capacity - header) / big_line;
        Log log(Level::warn, title, fresh(rows).string());
        std::string &text = log.*reach(LogText{});
        text.assign(header, 'h');
        for (std::size_t line = 0; line < lines; ++line)
          text.append(big_line - 1, 'a').append("\n");
        log.*reach(LogStart{}) = header;
        const std::string last =
            std::string(file_capacity - text.size() - 1 + past, 'n') + "\n";
        log.*reach(LogOut{}) = last;
        log.*reach(LogUnsaved{}) = true;
        (log.*reach(LogAddToFile{}))();
        return std::format("{} bytes, header kept {}, last line kept {}",
                           text.size(),
                           text.substr(0, header) == std::string(header, 'h'),
                           text.ends_with(last));
      });
      rows.row(std::format(
          "{} -> {}", past == 0 ? "the room exactly" : "a byte past it", said));
    }
  });
}

} // namespace VP::bench
