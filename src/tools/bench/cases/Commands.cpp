#include "runtime/Commands.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

// runtime/Commands.cpp: the one command port (V06). Each row makes a port of its own,
// registers commands like vulpen's own, of every placeholder kind, and runs a line, a
// script or a session through it; a handler keeps what each command was handed. The
// views are two fakes, the one vulpen started with and game, which it hosts; the files
// are the sandbox's, a row's folder shown as <here>.

namespace VP::bench {

struct CommandLogText {
  using type = std::string (CommandLog::*)(const std::filesystem::path &) const;
  friend constexpr type reach(CommandLogText);
};
template struct Reach<CommandLogText, &CommandLog::text>;
struct CommandLogViewsSince {
  using type = std::vector<std::string> (CommandLog::*)(std::size_t) const;
  friend constexpr type reach(CommandLogViewsSince);
};
template struct Reach<CommandLogViewsSince, &CommandLog::views_since>;

namespace domain {
// A usage a command registers with.
struct UsageText {};
// A line, as a person types it or a node sends it.
struct Line {};
} // namespace domain

template <> struct Gen<domain::UsageText> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"a b",
            "",
            "a",
            "A",
            "9a",
            "a-b c9",
            "a <name>",
            "a <nope>",
            "a <name> b",
            "<name> a",
            "a [<name>]",
            "a [<view>]",
            "a <value>...",
            "a [<value>...]",
            "a <value>... <name>",
            "a [<name>] <port> <port>",
            "a <file>...",
            "a <node> <port>",
            "a b c d e",
            "a  b",
            "a\tb",
            "a <<name>>",
            "a <name >",
            "a [<name>"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::Line> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"echo hi",
            "",
            "   ",
            "echo",
            "echo a b c",
            "nope",
            "node",
            "node add lamp",
            "node add a lamp",
            "node add a lamp more",
            "node add a.b lamp",
            "node set a x=1 y=2",
            "node set a",
            "x=1",
            "connect a.out b.in",
            "connect ab a.out b.in c.in",
            "connect game: show.picture",
            "game: echo hi",
            "nogame: echo hi",
            ": echo hi",
            ":",
            "game:",
            "file open a/b.txt",
            "file open ../x",
            "file open /abs/x",
            "file open a b",
            "files copy a b/c",
            "view save",
            "view save p/q",
            "view save p",
            "shared x",
            "game: shared x",
            "echo hi\r",
            "echo\thi",
            "  echo   hi  ",
            "ECHO hi",
            "echo-x",
            "refuse now",
            "nest echo inner",
            "nest nope",
            "nest refuse",
            "view switch",
            "view switch game",
            "view switch nogame",
            "log save",
            "source",
            std::string(300, 'a')};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    std::string line = all[random.below(all.size())];
    if (random.below(2) == 0)
      line += " " + all[random.below(all.size())];
    return line;
  }
};

namespace {

// The views a port looks in: the one vulpen started with, named by none, and game,
// which it hosts. What the port asks of them is kept, a line each.
class Views final : public ViewLookup {
public:
  explicit Views(FilePort &files) : _files(files) {
    _start.name = "start";
    _game.name = "game";
  }
  std::vector<std::string> asked;
  std::vector<std::string> unkept_files; // what unkept() answers for each view
  bool game_hosted = true;               // false once a close took it, as view close does
  bool played_alone = false;             // what played() answers: no app hosted

  const View *find(std::string_view name) override {
    return name.empty() ? &_start : name == "game" && game_hosted ? &_game : nullptr;
  }
  const View *unfolded(std::string_view name) override {
    return find(name);
  }
  std::span<const Cost> costs(std::string_view) override {
    return {};
  }
  std::span<const NodePort> ports(std::string_view) override {
    return {};
  }
  void write(std::string_view name) override {
    asked.push_back(std::format("write {}", shown(name)));
  }
  std::vector<std::string> names() override {
    return game_hosted ? std::vector<std::string>{"", "game"}
                       : std::vector<std::string>{""};
  }
  std::vector<std::string> unkept(std::string_view) override {
    return unkept_files;
  }
  void put_back(std::string_view name) override {
    asked.push_back(std::format("put back {}", shown(name)));
  }
  void replace(std::string_view name, View) override {
    asked.push_back(std::format("replace {}", shown(name)));
  }
  std::optional<std::string> host_of(std::string_view name) override {
    return name == "game" ? std::optional<std::string>("") : std::nullopt;
  }
  void check(std::string_view, const View &) override {}
  void begin_step(std::string_view line) override {
    asked.push_back("begin step " + escaped(line));
  }
  void end_step(std::optional<std::size_t> group) override {
    asked.push_back(group ? std::format("end step, group {}", *group)
                          : std::string("end step, nothing kept"));
  }
  FilePort &files() override {
    return _files;
  }
  std::size_t done() const override {
    return 0;
  }
  bool played() override {
    return played_alone;
  }

private:
  static std::string shown(std::string_view name) {
    return name.empty() ? "the start view" : std::string(name);
  }
  FilePort &_files;
  View _start;
  View _game;
};

// Keeps what each command was handed; refuse throws, nest sends its words as a line;
// each answers its own word, so which of two of one name ran shows.
class Handler final : public CommandHandler {
public:
  std::vector<std::string> handed;
  void command(Call &call) override {
    std::string words;
    for (const std::string_view word : call.arguments())
      words.append(words.empty() ? "" : ", ").append(escaped(word));
    handed.push_back(std::format("[{}]", words));
    if (!call.arguments().empty() && call.arguments().front() == "refuse")
      throw std::runtime_error("refused by its handler");
    if (named && !call.arguments().empty() && call.arguments().front().empty())
      call.keep_name(0, "chosen");
    if (nests && !call.arguments().empty()) {
      std::string line;
      for (const std::string_view word : call.arguments())
        line.append(line.empty() ? "" : " ").append(word);
      call.reply("nested: " + call.commands().send(line));
    }
    call.reply(said);
  }
  bool named = false;
  bool nests = false;
  std::string said = "ran";
};

// A port with commands like vulpen's, of every kind of placeholder.
struct Port {
  Log log{Level::warn, "bench"};
  SandboxFiles files;
  NoProcesses processes;
  Views views{files};
  Commands port{log, files, processes};
  Handler plain;
  Handler named;
  Handler nests;
  Handler of_game;

  Port() {
    named.named = true;
    nests.nests = true;
    of_game.said = "ran for game";
    port.look_in(views);
    port.add("echo <value>...", "says its words", plain);
    port.add("node add [<name>] <recipe>", "adds a node", named, Primitive::edit);
    port.add("node set <node> <word=value>...", "sets words", plain, Primitive::edit);
    port.add("connect [<name>] <port> <port>...", "connects", named, Primitive::edit);
    port.add("file open <path>", "opens a file", plain);
    port.add("files copy <path> <folder>", "copies", plain, Primitive::edit);
    port.add("view save [<project>]", "saves", plain, Primitive::save);
    port.add("refuse <value>", "refuses", plain, Primitive::edit);
    port.add("nest <value>...", "sends its words as a line", nests, Primitive::edit);
    // Two projects' nodes', as the binder registers a node's (by_node).
    port.add("shared <value>", "of two views", plain, Primitive::edit, "", true);
    port.add("shared <value>", "of two views", of_game, Primitive::edit, "game", true);
  }

  // What the line answered, or the refusal, what each handler was handed, what the views
  // were asked, and the session's log after it, the row's folder as <here>.
  // logged_from: where the log names the line's files from, the row's folder, or the
  // working folder for a line a node sends, whose paths count from there.
  std::string run(const std::function<std::string()> &line,
                  const std::filesystem::path &folder,
                  const std::filesystem::path &logged_from = {}) {
    std::string said;
    try {
      said = "answers " + escaped(line());
    } catch (const std::exception &failure) {
      said = "throws " + escaped(failure.what());
    }
    std::string handed;
    for (const Handler *const handler : {&plain, &named, &nests, &of_game})
      for (const std::string &each : handler->handed)
        handed.append(handed.empty() ? "" : " ").append(each);
    std::string asked;
    for (const std::string &each : views.asked)
      asked.append(asked.empty() ? "" : "; ").append(each);
    // What the line logged at warn or worse, as a refusal of a line typed or sent goes
    // there rather than to its sender.
    port.frame();
    std::string logged;
    for (const Logged &each : port.log())
      if (each.level <= Level::warn)
        logged.append(logged.empty() ? "" : "; ").append(escaped(each.text));
    return here(std::format("{}; handed {}; asked {};{} log {}",
                            said,
                            handed.empty() ? "nothing" : handed,
                            asked.empty() ? "nothing" : asked,
                            logged.empty() ? "" : " logged " + logged + ";",
                            escaped((port.session().*reach(CommandLogText{}))(
                                logged_from.empty() ? folder : logged_from))),
                folder);
  }
};

std::string
with_port(const Rows &rows,
          const std::function<std::string(Port &, const std::filesystem::path &)> &body) {
  const std::filesystem::path folder = planted(rows, {});
  return rows.quiet([&] {
    Port port;
    return body(port, folder);
  });
}

} // namespace

void cases(Bench &bench) {
  bench.sweep(
      "Commands.cpp/Commands::add",
      [](const Rows &rows, const std::string &usage) {
        return rows.quiet([&] {
          Port port;
          try {
            port.port.add(usage, "help", port.plain);
          } catch (const std::exception &failure) {
            return "throws " + escaped(failure.what());
          }
          std::string again;
          try {
            port.port.add(usage, "help", port.plain);
            again = "registers again";
          } catch (const std::exception &failure) {
            again = "again throws " + escaped(failure.what());
          }
          return std::format(
              "registers as {}; {}", escaped(port.port.usages().back().usage), again);
        });
      },
      domain::UsageText{});

  // A line run as a script's, through run(): what it answers or throws, as a command a
  // command sends meets it.
  bench.sweep(
      "Commands.cpp/Commands::run",
      [](const Rows &rows, const std::string &line) {
        return with_port(rows, [&](Port &port, const std::filesystem::path &folder) {
          return port.run([&] { return port.port.run(line, folder.string()); }, folder);
        });
      },
      domain::Line{});

  // A line a person typed at the row's folder: a step of its own, refused or not, each
  // view it changed written, and a refusal to the log rather than thrown.
  bench.sweep(
      "Commands.cpp/Commands::send_typed",
      [](const Rows &rows, const std::string &line) {
        return with_port(rows, [&](Port &port, const std::filesystem::path &folder) {
          return port.run([&] { return port.port.send_typed(line, folder.string()); },
                          folder);
        });
      },
      domain::Line{});

  // A line a node sends: no step, nothing written, a refusal to the log.
  bench.sweep(
      "Commands.cpp/Commands::send",
      [](const Rows &rows, const std::string &line) {
        return with_port(rows, [&](Port &port, const std::filesystem::path &folder) {
          return port.run([&] { return port.port.send(line); },
                          folder,
                          std::filesystem::current_path());
        });
      },
      domain::Line{});

  // Scripts in the sandbox: each line a group of its own, paths counted from the script's
  // folder, a refusal stopping it naming file and line, or passed over for its view.
  bench.check("Commands.cpp/Commands::source", [](Rows &rows) {
    struct Case {
      std::string_view said;
      std::vector<std::string> tree;
      std::string_view script;
      bool past_refusals = false;
    };
    const std::vector<Case> cases{
        {"two lines", {"s=echo one\nnode add a lamp\n"}, "s"},
        {"no last newline", {"s=echo one\necho two"}, "s"},
        {"lines of Windows", {"s=echo one\r\nnode add a lamp\r\n"}, "s"},
        {"a refusal in the middle", {"s=echo one\nnope\necho three\n"}, "s"},
        {"a refusal passed over",
         {"s=echo one\ngame: echo refuse\ngame: echo two\necho three\n"},
         "s",
         true},
        {"a refusal not passed over",
         {"s=echo one\ngame: echo refuse\ngame: echo two\necho three\n"},
         "s"},
        {"a path from its folder", {"d/", "d/s=file open x/y.txt\n"}, "d/s"},
        {"itself", {"s=source s\n"}, "s"},
        {"another that sources it", {"s=source t\n", "t=source s\n"}, "s"},
        {"none", {}, "s"},
        {"a folder", {"d/"}, "d"},
        {"empty", {"s="}, "s"},
        {"quit in the middle", {"s=echo one\nquit\necho three\n"}, "s"},
        {"blank lines", {"s=\n\n   \necho one\n"}, "s"},
    };
    for (const Case &each : cases) {
      const std::filesystem::path folder = planted(rows, each.tree);
      const std::string ran = rows.quiet([&] {
        Port port;
        return port.run(
            [&] {
              port.port.source(folder / each.script, each.past_refusals);
              return std::string();
            },
            folder);
      });
      rows.row(std::format("{} -> {}", each.said, ran));
    }
  });

  // The session written to a file, which source replays: refused over a file there not
  // named .log, or a folder; force writes over it; paths named from the log's folder.
  bench.check("Commands.cpp/log save", [](Rows &rows) {
    struct Case {
      std::string_view said;
      std::vector<std::string> tree;
      std::string_view line;
    };
    const std::vector<Case> cases{
        {"a new log", {}, "log save s.log"},
        {"over a log", {"s.log=old"}, "log save s.log"},
        {"over a file not named .log", {"s.txt=mine"}, "log save s.txt"},
        {"force over it", {"s.txt=mine"}, "log save force s.txt"},
        {"a folder", {"d/"}, "log save d"},
        {"in a folder, paths from there", {"d/"}, "log save d/s.log"},
    };
    for (const Case &each : cases) {
      const std::filesystem::path folder = planted(rows, each.tree);
      const std::string ran = rows.quiet([&] {
        Port port;
        port.port.send_typed("files copy a d/b", folder.string());
        port.port.send_typed("node add a lamp", folder.string());
        return port.run([&] { return port.port.run(each.line, folder.string()); },
                        folder);
      });
      // Escaped, as a log's lines in a file's text would end the row.
      rows.row(std::format(
          "{} -> {}; then {}", each.said, ran, escaped(here(tree_of(folder), folder))));
    }
  });

  // What the log keeps: each line a group, a part's answer joining the line it answers
  // within three frames, an undo taking a group back and a redo bringing it again; a
  // save's line left out of what a replay after a mark runs.
  bench.check("Commands.cpp/CommandLog", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::string said = rows.quiet([&] {
      Port port;
      std::string text;
      const auto step = [&](std::string_view what) {
        text +=
            std::format("{}: {}\n",
                        what,
                        escaped((port.port.session().*reach(CommandLogText{}))(folder)));
      };
      port.port.send_typed("node add a lamp", folder.string());
      step("a line typed");
      port.port.frame();
      port.port.send("node set a x=1");
      step("a part's answer a frame after");
      for (int frame = 0; frame < 4; ++frame)
        port.port.frame();
      port.port.send("node set a y=2");
      step("a part's line four frames after");
      port.port.send_typed("view save", folder.string());
      port.port.take_back(0, true);
      step("the first group taken back");
      port.port.take_back(0, false);
      step("and brought back");
      const std::array marks{CommandLog::Mark{.view = "", .kept = 1}};
      text += std::format("after the first kept, for the start view: {}\n",
                          escaped(port.port.session().after(marks, folder)));
      Names names;
      text += std::format(
          "views changed since 0: {}\n",
          show((port.port.session().*reach(CommandLogViewsSince{}))(0), names));
      return text;
    });
    for (const std::string_view line :
         std::views::split(said, '\n') | std::views::transform([](auto piece) {
           return std::string_view(piece.begin(), piece.end());
         }))
      if (!line.empty())
        rows.row(here(std::string(line), folder));
  });

  // A part's line joins the group of the line a person ran in the few frames before,
  // and only such a line, one that changed something, starts that window: not a line
  // typed that changed nothing, nor a part's line itself.
  bench.check("Commands.cpp/CommandLog, what a part's line joins", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::string said = rows.quiet([&] {
      std::string text;
      {
        Port port;
        port.port.send_typed("node add a lamp", folder.string());
        for (int frame = 0; frame < 4; ++frame)
          port.port.frame();
        port.port.send_typed("echo hi", folder.string());
        port.port.send("node set a x=1");
        port.port.send("node set a y=2");
        port.port.take_back(1, true);
        text +=
            std::format("a line typed that changed nothing four frames after the "
                        "last change, then two parts' lines, the second group taken "
                        "back: {}\n",
                        escaped((port.port.session().*reach(CommandLogText{}))(folder)));
      }
      {
        Port port;
        port.port.send_typed("node add a lamp", folder.string());
        for (int frame = 0; frame < 4; ++frame)
          port.port.frame();
        port.port.send("node set a x=1");
        port.port.send("node set a y=2");
        port.port.take_back(1, true);
        text +=
            std::format("two parts' lines in one frame, four after the last typed, "
                        "the second group taken back: {}\n",
                        escaped((port.port.session().*reach(CommandLogText{}))(folder)));
      }
      return text;
    });
    for (const auto piece : std::views::split(said, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(here(std::string(line), folder));
  });

  // quit: refused while a node holds work or a project's files differ from its last
  // save, each named; a node's hold lasts the frame after; quit force puts each such
  // project back. A run that plays projects alone refuses only for work a node holds,
  // and puts nothing back.
  bench.check("Commands.cpp/quit", [](Rows &rows) {
    struct Case {
      std::string_view said;
      std::function<void(Port &)> before;
      std::string_view line;
    };
    const std::vector<Case> cases{
        {"nothing held", [](Port &) {}, "quit"},
        {"a node holds work",
         [](Port &port) { port.port.hold("game:editor", "a.txt"); },
         "quit"},
        {"held a frame ago",
         [](Port &port) {
           port.port.hold("game:editor", "a.txt");
           port.port.frame();
         },
         "quit"},
        {"held two frames ago",
         [](Port &port) {
           port.port.hold("game:editor", "a.txt");
           port.port.frame();
           port.port.frame();
         },
         "quit"},
        {"held twice",
         [](Port &port) {
           port.port.hold("game:editor", "a.txt");
           port.port.hold("game:editor", "a.txt");
         },
         "quit"},
        {"files no save kept",
         [](Port &port) { port.views.unkept_files = {"a.txt", "b/"}; },
         "quit"},
        {"quit force over them",
         [](Port &port) {
           port.port.hold("game:editor", "a.txt");
           port.views.unkept_files = {"a.txt"};
         },
         "quit force"},
        // A run that plays projects alone keeps their files as they are, and ends.
        {"files no save kept, played alone",
         [](Port &port) {
           port.views.played_alone = true;
           port.views.unkept_files = {"a.txt", "b/"};
         },
         "quit"},
        {"a node holds work, played alone",
         [](Port &port) {
           port.views.played_alone = true;
           port.port.hold("game:editor", "a.txt");
         },
         "quit"},
        {"quit force over them, played alone",
         [](Port &port) {
           port.views.played_alone = true;
           port.port.hold("game:editor", "a.txt");
           port.views.unkept_files = {"a.txt"};
         },
         "quit force"},
    };
    for (const Case &each : cases) {
      const std::filesystem::path folder = planted(rows, {});
      const std::string ran = rows.quiet([&] {
        Port port;
        each.before(port);
        std::string said = port.run([&] { return port.port.run(each.line); }, folder);
        return said + std::format("; quitting {}", port.port.quitting());
      });
      rows.row(std::format("{} -> {}", each.said, ran));
    }
  });

  // The view lines go to: switched to a hosted one and back, and the nearest host once
  // the view it held goes.
  bench.check("Commands.cpp/view switch", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::string said = rows.quiet([&] {
      Port port;
      std::string text;
      for (const std::string_view line : {"view switch game",
                                          "view switch",
                                          "view switch nogame",
                                          "view switch game"}) {
        const std::string ran = port.run([&] { return port.port.run(line); }, folder);
        text += std::format(
            "{} -> {}; current {}\n", line, ran, escaped(port.port.current()));
      }
      // The current view closed: lines go to the nearest view that held it.
      port.views.game_hosted = false;
      text += std::format("game closed; current {}\n", escaped(port.port.current()));
      const std::string ran = port.run([&] { return port.port.run("shared x"); }, folder);
      text +=
          std::format("shared x -> {}; current {}\n", ran, escaped(port.port.current()));
      return text;
    });
    for (const auto piece : std::views::split(said, '\n'))
      if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
        rows.row(std::string(line));
  });

  // One name, registered by two of: the engine, an app's node, a node of the start
  // project, a node of game. Which pairs register, and with an app's node's and game's
  // both registered, which a line reaches: game's for a line naming game, the app's for
  // any other, game current or not, as the port addresses only the view a line names
  // and the command line names the current one in each line typed.
  bench.check("Commands.cpp/a command several nodes register", [](Rows &rows) {
    struct Owner {
      std::string_view said;
      std::optional<std::string_view> view;
      bool by_node;
    };
    const std::vector<Owner> owners{{"the engine's", std::nullopt, false},
                                    {"an app's node's", std::nullopt, true},
                                    {"the start project's node's", "", true},
                                    {"game's node's", "game", true}};
    for (const Owner &first : owners)
      for (const Owner &second : owners)
        rows.row(std::format("{} then {}: {}", first.said, second.said, rows.quiet([&] {
          Port port;
          port.port.add("twice <value>",
                        "help",
                        port.plain,
                        Primitive::no,
                        first.view,
                        first.by_node);
          try {
            port.port.add("twice <value>",
                          "help",
                          port.plain,
                          Primitive::no,
                          second.view,
                          second.by_node);
            return std::string("both register");
          } catch (const std::exception &failure) {
            return "the second throws " + escaped(failure.what());
          }
        })));
    const std::filesystem::path folder = planted(rows, {});
    const std::vector<std::vector<std::string_view>> lines{
        {"zoom x"},
        {": zoom x"},
        {"game: zoom x"},
        {"nogame: zoom x"},
        {"view switch game", "zoom x"},
        {"view switch game", ": zoom x"}};
    for (const std::vector<std::string_view> &each : lines)
      rows.row(rows.quiet([&] {
        Port port;
        Handler app;
        app.said = "ran for the app";
        port.port.add("zoom <value>", "zooms", app, Primitive::no, std::nullopt, true);
        port.port.add("zoom <value>", "zooms", port.of_game, Primitive::no, "game", true);
        std::string text;
        for (const std::string_view line : each)
          text = std::format("{}{} -> {}",
                             text.empty() ? "" : text + ", then ",
                             line,
                             port.run([&] { return port.port.run(line); }, folder));
        return text;
      }));
  });

  // A log's lines run again: each refusal kept with its line, nothing kept in the
  // session.
  bench.check("Commands.cpp/Commands::replay", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const std::string said = rows.quiet([&] {
      Port port;
      std::vector<std::string> refused;
      const std::string ran = port.run(
          [&] {
            refused =
                port.port.replay("echo one\nnope\n\nnode add a lamp\ngame: refuse x\n");
            return std::string();
          },
          folder);
      Names names;
      return std::format("{}; refused {}", ran, here(show(refused, names), folder));
    });
    rows.row("a log of five lines -> " + said);
  });

  bench.sweep("Commands.cpp/resolved_from",
              [](const std::filesystem::path &folder, const std::filesystem::path &file) {
                return resolved_from(folder, file);
              });
  bench.sweep("Commands.cpp/command_word", command_word, domain::Word{});
  bench.sweep("Commands.cpp/placeholder", placeholder, domain::Word{});
  // words_of(const Node &) of View.h shares the name.
  bench.sweep(
      "Commands.cpp/words_of",
      [](std::string_view line) { return words_of(line); },
      domain::Line{});

  for (const char *const id : {"Commands.cpp/Commands::source",
                               "Commands.cpp/log save",
                               "Commands.cpp/resolved_from"})
    bench.leaves_out(id,
                     "locale",
                     "the messages carry the C library's, in the process's "
                     "language");
  for (const char *const id :
       {"Commands.cpp/Commands::run", "Commands.cpp/Commands::send_typed"})
    bench.leaves_out(id,
                     "threads",
                     "a project's path counts from where vulpen started, the process's "
                     "folder, which the log names from the folder a line was typed at: "
                     "each thread's sandbox lies a folder deeper, so the way there is "
                     "one .. longer");
}

} // namespace VP::bench
