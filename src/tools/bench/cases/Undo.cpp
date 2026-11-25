#include "runtime/Undo.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// runtime/Undo.cpp: a line's changes to files, taken back and run again (V08). Each row
// plants a tree in a folder of its own, runs one step through the undo's file port, as a
// line a person typed runs, then undo and redo, and shows the tree after each, with what
// each answered. The views are none: a step changes files alone here.

namespace VP::bench {

namespace domain {
// A path in the row's folder, never above it, as an undo that goes wrong there would take
// the sandbox's other rows with it.
struct InsidePath {};
} // namespace domain

template <> struct Gen<domain::InsidePath> {
  using type = std::string;
  static std::vector<std::string> edges() {
    std::vector<std::string> all = Gen<domain::TreePath>::edges();
    std::erase(all, "..");
    return all;
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())];
  }
};

namespace {

using Change = std::function<void(FilePort &, const std::filesystem::path &)>;

struct Session {
  Log log{Level::error, "bench"};
  SandboxFiles files;
  NoProcesses processes;
  NoViews views{files};
  Commands port{log, files, processes};
  Undo undo{files, views, port};

  // The step's change, then something else between it and the undo; what each of the
  // three left.
  std::string run(const std::filesystem::path &folder,
                  const Change &change,
                  const std::function<void()> &between = {}) {
    undo.begin("the line");
    std::string said;
    try {
      change(undo.files(), folder);
      said = "done";
    } catch (const std::exception &failure) {
      said = "throws " + escaped(here(failure.what(), folder));
    }
    undo.end(std::nullopt);
    said += "; then " + tree_of(folder);
    if (between)
      between();
    for (const bool again : {false, true}) {
      ReplyCall call;
      said += again ? "; redo " : "; undo ";
      said += after(folder, [&] { undo.take_back(call, again); });
    }
    return said;
  }
};

std::string
stepped(const Rows &rows,
        const std::vector<std::string> &tree,
        const Change &change,
        const std::function<void(const std::filesystem::path &)> &between = {}) {
  const std::filesystem::path folder = planted(rows, tree);
  return rows.quiet([&] {
    Session session;
    return session.run(
        folder, change, between ? [&] { between(folder); } : std::function<void()>{});
  });
}

} // namespace

void cases(Bench &bench) {
  bench.sweep(
      "Undo.cpp/a save",
      [](const Rows &rows, const std::vector<std::string> &tree, std::string_view file) {
        return stepped(
            rows, tree, [&](FilePort &files, const std::filesystem::path &folder) {
              files.save((folder / file).string(), "new");
            });
      },
      domain::Tree{},
      domain::InsidePath{});
  bench.sweep(
      "Undo.cpp/a remove",
      [](const Rows &rows, const std::vector<std::string> &tree, std::string_view file) {
        return stepped(
            rows, tree, [&](FilePort &files, const std::filesystem::path &folder) {
              files.remove((folder / file).string());
            });
      },
      domain::Tree{},
      domain::InsidePath{});
  bench.sweep(
      "Undo.cpp/a rename",
      [](const Rows &rows,
         const std::vector<std::string> &tree,
         std::string_view from,
         std::string_view to) {
        return stepped(
            rows, tree, [&](FilePort &files, const std::filesystem::path &folder) {
              files.rename((folder / from).string(), (folder / to).string());
            });
      },
      domain::Tree{},
      domain::InsidePath{},
      domain::InsidePath{});
  bench.sweep(
      "Undo.cpp/a copy",
      [](const Rows &rows,
         const std::vector<std::string> &tree,
         std::string_view from,
         std::string_view to) {
        return stepped(
            rows, tree, [&](FilePort &files, const std::filesystem::path &folder) {
              files.copy((folder / from).string(), (folder / to).string());
            });
      },
      domain::Tree{},
      domain::InsidePath{},
      domain::InsidePath{});
  bench.sweep(
      "Undo.cpp/a folder made",
      [](const Rows &rows, const std::vector<std::string> &tree, std::string_view made) {
        return stepped(
            rows, tree, [&](FilePort &files, const std::filesystem::path &folder) {
              files.make((folder / made).string());
            });
      },
      domain::Tree{},
      domain::InsidePath{});

  // A person's own editor saves a file between the step and the undo, as the undo's
  // comment says it refuses to lose: "What would lose a change made since is refused
  // before anything changes."
  bench.check("Undo.cpp/a change made since", [](Rows &rows) {
    struct Case {
      std::string_view said;
      std::vector<std::string> tree;
      Change change;
      std::string_view since;
    };
    const std::vector<Case> cases{
        {"a save of a, then a's text changed",
         {"a=1"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.save((folder / "a").string(), "2");
         },
         "a"},
        {"a folder d made, then d/notes saved in it",
         {},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.make((folder / "d").string());
         },
         "d/notes"},
        {"d copied to e, then e/notes saved in it",
         {"d/", "d/b=2"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.copy((folder / "d").string(), (folder / "e").string());
         },
         "e/notes"},
        {"a renamed to b, then a made again",
         {"a=1"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.rename((folder / "a").string(), (folder / "b").string());
         },
         "a"},
        {"a copied onto b, which is there, refused; then c saved",
         {"a=1", "b/", "b/keep=mine"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.copy((folder / "a").string(), (folder / "b").string());
         },
         "c"},
        {"d made where d is, refused; then c saved",
         {"d/", "d/keep=mine"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.make((folder / "d").string());
         },
         "c"},
    };
    for (const Case &each : cases) {
      const std::string since(each.since);
      rows.row(std::format(
          "{} -> {}",
          each.said,
          stepped(rows, each.tree, each.change, [&](const std::filesystem::path &folder) {
            Files::save(folder / since, "mine");
          })));
    }
  });

  // A step of several changes: what a later one puts inside a folder an earlier one made
  // is the step's own, so undo takes the folder back whole, refusing nothing.
  bench.check("Undo.cpp/a step of several changes", [](Rows &rows) {
    struct Case {
      std::string_view said;
      std::vector<std::string> tree;
      Change change;
    };
    const std::vector<Case> cases{
        {"d made, then a copied into it",
         {"a=1"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.make((folder / "d").string());
           files.copy((folder / "a").string(), (folder / "d" / "a").string());
         }},
        {"d made, then d/x saved in it, as node new makes a node",
         {},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.make((folder / "d").string());
           files.save((folder / "d" / "x").string(), "1");
         }},
        {"d made, d/x saved, then renamed to d/y",
         {},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.make((folder / "d").string());
           files.save((folder / "d" / "x").string(), "1");
           files.rename((folder / "d" / "x").string(), (folder / "d" / "y").string());
         }},
        {"a saved twice",
         {"a=1"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.save((folder / "a").string(), "2");
           files.save((folder / "a").string(), "3");
         }},
        {"a renamed to b, then a saved anew",
         {"a=1"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.rename((folder / "a").string(), (folder / "b").string());
           files.save((folder / "a").string(), "new");
         }},
        {"d renamed to e, then e/x saved",
         {"d/", "d/x=1"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.rename((folder / "d").string(), (folder / "e").string());
           files.save((folder / "e" / "x").string(), "2");
         }},
        {"a removed, then b renamed to a",
         {"a=1", "b=2"},
         [](FilePort &files, const std::filesystem::path &folder) {
           files.remove((folder / "a").string());
           files.rename((folder / "b").string(), (folder / "a").string());
         }},
    };
    for (const Case &each : cases)
      rows.row(std::format("{} -> {}", each.said, stepped(rows, each.tree, each.change)));
  });

  for (const char *const id : {"Undo.cpp/a save",
                               "Undo.cpp/a remove",
                               "Undo.cpp/a rename",
                               "Undo.cpp/a copy",
                               "Undo.cpp/a folder made"})
    bench.leaves_out(id,
                     "locale",
                     "the messages carry the C library's, in the process's "
                     "language");
}

} // namespace VP::bench
