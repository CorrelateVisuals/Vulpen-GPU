#include "projects/Projects.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/projects: the commands that list, make, host, copy, rename and
// remove projects, views of their own in the projects' folder or in a folder there that
// groups some, by a project's name or a folder's path; an app's name hosts the app
// beside the projects again. Each line it sends goes to the engine's view commands, and
// each view it hosts becomes the current one.

namespace VP::bench {

namespace {

// The commands in the order bind registers them, each its index.
enum Line : std::uint32_t { made, load, save_as, renamed, removed, listed };

// A tree as a checkout holds one: the projects wave, triangles/triangle and
// triangles/triangle-params, a folder holding no view, a repository holding its project
// in a folder, a folder holding two; the apps ide and cli beside the library; and the
// part's own folder, from which its params count.
std::filesystem::path tree(const Rows &rows) {
  return planted(rows,
                 {"projects/wave/view.vlp=[manifest]\nversion = 1\n",
                  "projects/wave/wave/Wave.comp=",
                  "projects/triangles/triangle/view.vlp=[manifest]\nversion = 1\n",
                  "projects/triangles/triangle-params/view.vlp=[manifest]\nversion = 1\n",
                  "projects/empty",
                  "projects/repo/inner/view.vlp=[manifest]\nversion = 1\n",
                  "projects/two/a/view.vlp=[manifest]\nversion = 1\n",
                  "projects/two/b/view.vlp=[manifest]\nversion = 1\n",
                  "recipes/ide/view.vlp=[manifest]\nversion = 1\n",
                  "recipes/cli/view.vlp=[manifest]\nversion = 1\n",
                  "recipes/library/parts/projects/view.vlp=[manifest]\nversion = 1\n"});
}

struct Framed {
  Running<Projects> node{{{"folder", "../../../../projects"}, {"apps", "../../.."}}};
  FakeCommands commands;
  SandboxFiles files;
  std::filesystem::path place;
  // The views a line finds by name: the ide vulpen started with, hosting those named.
  View top;
  std::map<std::string, View, std::less<>> hosted;

  explicit Framed(const Rows &rows) : place(tree(rows)) {
    node.bind.folder_path =
        (place / "recipes" / "library" / "parts" / "projects").string();
    node.bound();
    top.name = "";
    top.file = place / "recipes" / "ide" / "view.vlp";
    top.children.push_back({.name = "ide", .file = top.file});
    commands.answer = "removed";
  }
  // A view hosted beside the ide, from its folder.
  View &host(std::string name, const std::filesystem::path &folder) {
    View view;
    view.name = name;
    view.file = folder / "view.vlp";
    top.children.push_back({.name = name, .file = view.file});
    return hosted[name] = std::move(view);
  }
  // A line of a command at the view addressed: what it sent and answered, or what it
  // refused; the paths in it as <here>, in either form.
  std::string
  ran(Line line, std::vector<std::string> words, const View &addressed, Rows &rows) {
    FakeCall call;
    call.running = {line};
    call.words = std::move(words);
    call.addressed = addressed;
    call.views = hosted;
    call.views[""] = top;
    call.command_port = &commands;
    call.file_port = &files;
    commands.sent.clear();
    std::string said;
    try {
      node.called(call);
      said = std::format("sent {}; answered {}",
                         show(commands.sent, rows.names()),
                         show(call.replies, rows.names()));
    } catch (...) {
      said = thrown();
    }
    // As a person reads a path, from where vulpen started, which the cwd context moves.
    const std::string from_start =
        place.lexically_proximate(std::filesystem::current_path()).generic_string();
    for (std::size_t at = said.find(from_start);
         !from_start.empty() && at != std::string::npos;
         at = said.find(from_start, at))
      said.replace(at, from_start.size(), "<here>");
    return here(said, place);
  }
  std::string ran(Line line, std::vector<std::string> words, Rows &rows) {
    return ran(line, std::move(words), top, rows);
  }
  std::string projects_tree() const {
    return tree_of(place / "projects");
  }
};

} // namespace

void cases(Bench &bench) {
  bench.check("Projects.cpp/check_name", [](Rows &rows) {
    for (const std::string_view name : {"wave", "a-b_1", "", "a b", "a.b", "a/b", "é"}) {
      std::string said = "passes";
      try {
        check_name(name);
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", escaped(name), said));
    }
  });
  bench.check("Projects.cpp/check_buildable", [](Rows &rows) {
    for (const std::string_view name :
         {"wave", "a.b+c-d_e", "", "a b", "a/b", "é", "a:b"}) {
      std::string said = "passes";
      try {
        check_buildable(name);
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", escaped(name), said));
    }
  });

  // Its commands, each with its usage and help (RV04).
  bench.check("Projects.cpp/Projects::bind", [](Rows &rows) {
    Framed frame(rows);
    for (const std::string &asked : frame.node.bind.asked)
      rows.row(asked);
  });

  // The projects by their folders from the projects' folder: a view there, or one in a
  // folder there that groups some; no folder holding none, and the files beside them
  // left out.
  bench.check("Projects.cpp/Projects::projects", [](Rows &rows) {
    Framed frame(rows);
    rows.row("the tree -> " + frame.ran(listed, {}, rows));
    Files::save(frame.place / "projects" / "notes.txt", "");
    Files::make(frame.place / "projects" / "triangles" / "deeper" / "inner");
    Files::save(frame.place / "projects" / "triangles" / "deeper" / "inner" / "view.vlp",
                "");
    rows.row("a file beside them, and a view two folders down -> " +
             frame.ran(listed, {}, rows));
  });

  // A project by its name, a group's too, or a folder by its path, its view.vlp's too;
  // one hosted from that folder already switched to; a repository's one folder holding
  // a view; an app by its name, beside the projects; each refused where it names none,
  // two, or a view hosted under that name from elsewhere.
  bench.check("Projects.cpp/Projects::host", [](Rows &rows) {
    Framed frame(rows);
    const auto load_of = [&](std::string_view what, std::vector<std::string> words) {
      rows.row(std::format("{} -> {}", what, frame.ran(load, std::move(words), rows)));
    };
    load_of("wave", {"wave"});
    load_of("triangle-params, in a group", {"triangle-params"});
    load_of("the path of wave's folder", {(frame.place / "projects" / "wave").string()});
    load_of("the path of wave's view.vlp",
            {(frame.place / "projects" / "wave" / "view.vlp").string()});
    load_of("the path of wave's folder, a / after it",
            {(frame.place / "projects" / "wave" / "").string()});
    load_of("repo, whose one folder holds a view", {"repo"});
    load_of("two, whose two folders hold a view each", {"two"});
    load_of("empty, holding none", {"empty"});
    load_of("gone, no folder", {"gone"});
    load_of("a b, no name", {"a b"});
    load_of("cli, an app", {"cli"});
    load_of("ide, the app vulpen started with", {"ide"});
    load_of("the projects' folder by its path", {(frame.place / "projects").string()});
    frame.host("wave", frame.place / "projects" / "wave");
    load_of("wave, hosted from its folder", {"wave"});
    frame.hosted["wave"].file = frame.place / "elsewhere" / "view.vlp";
    load_of("wave, a view of that name hosted from elsewhere", {"wave"});
    frame.host("cli", frame.place / "recipes" / "cli");
    load_of("cli, hosted already", {"cli"});
  });

  // A new view: with no name the first free project1 on; a name or a path a folder takes;
  // refused where a view is there already, the name is a project's in a group, or the
  // folder's name cannot be built.
  bench.check("Projects.cpp/Projects::free_project", [](Rows &rows) {
    Framed frame(rows);
    rows.row("none -> " + frame.ran(made, {}, rows));
    Files::make(frame.place / "projects" / "project1");
    frame.host("project2", frame.place / "elsewhere" / "project2");
    Files::make(frame.place / "projects" / "triangles" / "project3");
    Files::save(frame.place / "projects" / "triangles" / "project3" / "view.vlp", "");
    rows.row("none, project1 a folder, project2 hosted, project3 a group's -> " +
             frame.ran(made, {}, rows));
  });
  bench.check("Projects.cpp/Projects::folder_of", [](Rows &rows) {
    Framed frame(rows);
    const auto made_of = [&](std::string_view what, std::vector<std::string> words) {
      rows.row(std::format("{} -> {}", what, frame.ran(made, std::move(words), rows)));
    };
    made_of("fresh", {"fresh"});
    made_of("wave, a view there", {"wave"});
    made_of("triangle, a group's", {"triangle"});
    made_of("empty, a folder holding none", {"empty"});
    made_of("a path elsewhere", {(frame.place / "elsewhere" / "mine").string()});
    made_of("a path whose folder is my proj", {(frame.place / "my proj").string()});
    made_of("a path whose folder is a.b, no project's name",
            {(frame.place / "a.b").string()});
    made_of("a path whose folder is c++", {(frame.place / "c++").string()});
    made_of("a path to a view.vlp", {(frame.place / "new" / "view.vlp").string()});
    made_of("a path named triangle elsewhere",
            {(frame.place / "elsewhere" / "triangle").string()});
    frame.host("fresh", frame.place / "elsewhere" / "fresh");
    made_of("fresh, a view hosted under that name", {"fresh"});
  });

  // A copy of the view addressed, its edits too, into a new project, numbered on with no
  // name, which runs in its place; refused for the start view, a view a project hosts, an
  // app, a name taken, and a view hosting another from its folder.
  bench.check("Projects.cpp/Projects::save_as", [](Rows &rows) {
    const auto copied =
        [&](std::string_view what, std::vector<std::string> words, const auto &setup) {
          Framed frame(rows);
          View &wave = frame.host("wave", frame.place / "projects" / "wave");
          wave.nodes.push_back(worlds::node("edited"));
          View addressed = setup(frame, wave);
          const std::string said = frame.ran(save_as, std::move(words), addressed, rows);
          rows.row(std::format(
              "{} -> {}; then {}", what, said, here(frame.projects_tree(), frame.place)));
        };
    const auto same = [](Framed &, View &wave) { return wave; };
    copied("wave, no name", {}, same);
    copied("wave as copy", {"copy"}, same);
    copied("wave as triangle, a project's name", {"triangle"}, same);
    copied("wave as a b", {"a b"}, same);
    copied("wave2, no name, wave3 taken", {}, [](Framed &frame, View &) {
      Files::copy(frame.place / "projects" / "wave", frame.place / "projects" / "wave2");
      Files::make(frame.place / "projects" / "wave3");
      Files::save(frame.place / "projects" / "wave3" / "view.vlp", "");
      View &wave2 = frame.host("wave2", frame.place / "projects" / "wave2");
      wave2.nodes.push_back(worlds::node("edited"));
      return wave2;
    });
    copied("the start view", {}, [](Framed &frame, View &) { return frame.top; });
    copied("a view wave hosts", {}, [](Framed &frame, View &) {
      View inner;
      inner.name = "inner";
      inner.file = frame.place / "projects" / "wave" / "inner" / "view.vlp";
      return inner;
    });
    copied("the cli, an app", {}, [](Framed &frame, View &) {
      return frame.host("cli", frame.place / "recipes" / "cli");
    });
    copied("wave hosting a view from its folder", {}, [](Framed &frame, View &wave) {
      wave.children.push_back(
          {.name = "inside",
           .file = frame.place / "projects" / "wave" / "in" / "view.vlp"});
      return wave;
    });
  });

  // The view addressed saved, closed, its folder renamed where it is, and hosted under
  // the new name; refused for the start view, an app and a name taken.
  bench.check("Projects.cpp/Projects::rename", [](Rows &rows) {
    const auto renaming = [&](std::string_view what, std::string name, bool app = false) {
      Framed frame(rows);
      const View addressed =
          app ? frame.host("cli", frame.place / "recipes" / "cli")
              : frame.host("triangle",
                           frame.place / "projects" / "triangles" / "triangle");
      const std::string said = frame.ran(renamed, {std::move(name)}, addressed, rows);
      rows.row(std::format(
          "{} -> {}; then {}", what, said, here(frame.projects_tree(), frame.place)));
    };
    renaming("triangle as tri", "tri");
    renaming("triangle as wave, a project's name", "wave");
    renaming("triangle as a/b", "a/b");
    renaming("the cli as mine", "mine", true);
    Framed top(rows);
    rows.row("the start view -> " + top.ran(renamed, {"mine"}, rows));
  });

  // By a project's name alone: closed first where it is hosted, then deleted by the files
  // part's line, whose answer it gives; refused for a path or a name no project has.
  bench.check("Projects.cpp/Projects::remove", [](Rows &rows) {
    Framed frame(rows);
    const auto removing = [&](std::string_view what, std::string name) {
      rows.row(
          std::format("{} -> {}", what, frame.ran(removed, {std::move(name)}, rows)));
    };
    removing("wave, not hosted", "wave");
    removing("triangle, a group's", "triangle");
    removing("a path", (frame.place / "projects" / "wave").string());
    removing("gone", "gone");
    removing("empty, holding no view", "empty");
    frame.host("wave", frame.place / "projects" / "wave");
    removing("wave, hosted", "wave");
    frame.hosted["wave"].file = frame.place / "elsewhere" / "view.vlp";
    removing("wave, a view of that name hosted from elsewhere", "wave");
  });
}

} // namespace VP::bench
