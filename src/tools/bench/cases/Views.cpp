#include "runtime/Views.cpp"

#include "baseclasses/Passes.h"
#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Gpu.h"
#include "tools/bench/Stage.h"
#include "tools/bench/Worlds.h"

#include <format>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// runtime/Views.cpp: every view a run hosts, as a person's lines change them. Each row
// runs a session on copies of the wave project in a folder of its own in the sandbox,
// never the tree's, as Runtime::start wires it (bench/Stage.h): lines typed at the
// copy's folder, each answered or refused, then the views as they are, rebuilt as the
// next frame rebuilds them, and the copy's manifest. Graph edits, undo and redo, view
// save, a view hosted beside it and closed again. The frames' rows go on as a run goes
// before each frame: which views rest, how many of their nodes run, and the passes the
// frame gathers, where each view's draws go.

namespace VP::bench {

namespace {

// A row's text with its folder as <here>, as its absolute path and as the answers name
// it, from the working folder.
std::string here_too(std::string text, const std::filesystem::path &folder) {
  const std::string relative =
      folder.lexically_relative(std::filesystem::current_path()).generic_string();
  for (std::size_t at = text.find(relative); !relative.empty() && at != std::string::npos;
       at = text.find(relative, at + 6))
    text.replace(at, relative.size(), "<here>");
  return here(std::move(text), folder);
}

// What a project's walk kept: its files' paths, its folders, and what it left out.
std::string kept_text(const Kept &kept) {
  std::vector<std::string> files;
  for (const auto &[path, text] : kept.files)
    files.push_back(path);
  return std::format("files [{}]; folders [{}]; left [{}]",
                     joined(files, ", "),
                     joined(kept.folders, ", "),
                     joined(kept.left, ", "));
}

// The views and their nodes, the start view's first, each schedule's state.
std::string views_now(Views &views) {
  ViewLookup &lookup = views;
  std::string text;
  for (const std::string &name : lookup.names()) {
    const View *const view = lookup.find(name);
    if (view == nullptr)
      continue;
    std::string nodes;
    for (const Node &node : view->nodes)
      nodes.append(nodes.empty() ? "" : " ").append(node.name);
    text += std::format("{}{} [{}]",
                        text.empty() ? "" : "; ",
                        name.empty() ? std::string("start") : name,
                        nodes);
  }
  std::string ok;
  for (const Schedule *const schedule : views.schedules())
    ok.append(ok.empty() ? "" : ",").append(schedule->ok() ? "ok" : "not ok");
  return text + " (" + ok + ")";
}

// Lines typed one after another at the folder of the first copy, each a row: what it
// answered, what was logged at warn or worse, the views after, and the manifest's text.
void session(Rows &rows, const std::vector<std::string_view> &lines) {
  on_window(rows, [&](Gpu &, const Window &window) {
    const std::filesystem::path folder = planted(rows, {});
    copy_built(rows.repo() / "projects" / "wave", folder / "wave");
    copy_built(rows.repo() / "projects" / "triangles" / "triangle", folder / "triangle");
    auto stage = rows.quiet([&] { return std::make_unique<Stage>(window); });
    auto run =
        rows.quiet([&] { return std::make_unique<Session>(*stage, debug_mirror(rows)); });
    const std::string started = rows.quiet([&] {
      try {
        run->start(*stage, folder / "wave" / "view.vlp");
        return std::string("started");
      } catch (...) {
        return thrown();
      }
    });
    rows.row(here_too(std::format("00 {}: {}",
                                  started,
                                  run->views ? views_now(*run->views) : std::string()),
                      folder));
    if (!run->views)
      return;
    std::size_t step = 0;
    for (const std::string_view line : lines) {
      const std::string said = rows.quiet([&] {
        const std::string answer =
            run->commands.send_typed(line, (folder / "wave").string());
        run->views->rebuild();
        run->commands.frame();
        std::string logged;
        for (const Logged &each : run->commands.log())
          if (each.level <= Level::warn)
            logged.append(logged.empty() ? "" : "; ").append(escaped(each.text));
        return std::format("{:02} {} -> {}{}; views {}; manifest {}",
                           ++step,
                           line,
                           escaped(answer),
                           logged.empty() ? "" : "; logged " + logged,
                           views_now(*run->views),
                           escaped(Files::text(folder / "wave" / "view.vlp")));
      });
      rows.row(here_too(said, folder));
    }
    rows.quiet([&] {
      run.reset();
      stage.reset();
      return 0;
    });
  });
}

// A frame's passes as gather lays them out: a dispatch by its workgroups, a draw by its
// vertices, into the window or an image, each image numbered as the frame first meets
// it; a clear is a draw of nothing. r and w count the buffers each reads and writes, so
// two draws of as many vertices tell apart.
std::string passes_text(const std::vector<Pass> &passes) {
  std::vector<const Offscreen *> images;
  std::vector<std::string> said;
  for (const Pass &pass : passes) {
    const std::string buffers =
        std::format("r{} w{}", pass.reads.size(), pass.writes.size());
    if (pass.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
      said.push_back(std::format(
          "dispatch {} {}{}", pass.groups, buffers, pass.stores ? " stores" : ""));
      continue;
    }
    std::string into = "window";
    if (pass.offscreen) {
      if (std::ranges::find(images, pass.offscreen) == images.end())
        images.push_back(pass.offscreen);
      into = std::format("image {}",
                         std::ranges::find(images, pass.offscreen) - images.begin() + 1);
    }
    said.push_back(
        pass.vertex_count == 0 && pass.instance_count == 0
            ? "clear " + into
            : std::format("draw {} {} into {}", pass.vertex_count, buffers, into));
  }
  return said.empty() ? std::string("none") : joined(said);
}

// Each view as a frame found it: its nodes, its connections, and its schedule: resting,
// or how many of its nodes run of all, and whether it is ok.
std::string framed_views(Views &views) {
  ViewLookup &lookup = views;
  const std::span<Schedule *const> schedules = views.schedules();
  const std::vector<std::string> names = lookup.names();
  if (names.size() != schedules.size())
    return std::format("{} views, {} schedules", names.size(), schedules.size());
  std::string text;
  for (std::size_t at = 0; at < names.size(); ++at) {
    const View &view = *lookup.find(names[at]);
    std::string nodes;
    for (const Node &node : view.nodes)
      nodes.append(nodes.empty() ? "" : " ").append(node.name);
    for (const Connection &connection : view.connections) {
      std::string to;
      for (const Endpoint &end : connection.to)
        to.append(to.empty() ? "" : ",").append(end.text());
      nodes += std::format(" | {} {}>{}", connection.name, connection.from.text(), to);
    }
    const Schedule &schedule = *schedules[at];
    text += std::format(
        "{}{} [{}] {}{}",
        text.empty() ? "" : "; ",
        names[at].empty() ? std::string("start") : names[at],
        nodes,
        schedule.resting()
            ? std::string("rests")
            : std::format("runs {} of {}", schedule.running(), schedule.costs().size()),
        schedule.ok() ? "" : ", not ok");
  }
  return text;
}

// Lines one after another, as session's, on copies of wave, the triangle, its computed
// twin and its points, ray-cubes and the fail-loud view, each followed by what a run
// does before a frame
// (Runtime::prepare): the rebuild, the pull with what the frame before read, and the
// passes gathered; a row each, with the views and the passes. A line starting with # is
// the bench's own, between two frames: "#unread view/node/port" says that input read
// nothing the frame before (Cook::unread), "#read" takes that back; "#write file text"
// writes a file of the folder, as an editor does; "#reload view" is what a live build
// reloads, the view the build tree mirrors by that name.
void framed(Rows &rows,
            const std::vector<std::string> &tree,
            const std::vector<std::string_view> &lines) {
  on_window(rows, [&](Gpu &, const Window &window) {
    const std::filesystem::path folder = planted(rows, tree);
    for (const char *const project : {"projects/wave",
                                      "projects/triangles/triangle",
                                      "projects/triangles/triangle-compute",
                                      "projects/triangles/triangle-points",
                                      "projects/ray-cubes",
                                      "src/tools/tests/mistakes"})
      copy_built(rows.repo() / project,
                 folder / std::filesystem::path(project).filename());
    auto stage = rows.quiet([&] { return std::make_unique<Stage>(window); });
    auto run =
        rows.quiet([&] { return std::make_unique<Session>(*stage, debug_mirror(rows)); });
    const std::string started = rows.quiet([&] {
      try {
        run->start(*stage, folder / "wave" / "view.vlp");
        return std::string("started");
      } catch (...) {
        return thrown();
      }
    });
    std::set<std::string, std::less<>> unread;
    const Views::Unread reads =
        [&](std::string_view view, std::string_view node, std::string_view port) {
          return unread.contains(std::format("{}/{}/{}", view, node, port));
        };
    std::vector<Pass> passes;
    const auto frame = [&] {
      Views &views = *run->views;
      const bool rebuilt = views.rebuild();
      const bool pulled = views.pull(reads, rebuilt);
      passes.clear();
      views.gather(passes);
      run->commands.frame();
      std::string logged;
      for (const Logged &each : run->commands.log())
        if (each.level <= Level::warn)
          logged.append(logged.empty() ? "" : "; ").append(escaped(each.text));
      return std::format("{}rebuilt {}, pulled {}; views {}; passes {}",
                         logged.empty() ? "" : "logged " + logged + "; ",
                         rebuilt,
                         pulled,
                         framed_views(views),
                         passes_text(passes));
    };
    const auto act = [&](std::string_view line) -> std::string {
      if (line.starts_with("#unread "))
        unread.emplace(line.substr(8));
      else if (line.starts_with("#read "))
        unread.erase(std::string(line.substr(6)));
      else if (line.starts_with("#reload "))
        run->views->reload(std::vector<std::string>{std::string(line.substr(8))});
      else if (line.starts_with("#write ")) {
        const std::string_view rest = line.substr(7);
        const std::size_t space = rest.find(' ');
        Files::save(folder / rest.substr(0, space), rest.substr(space + 1));
      } else
        return " -> " +
               escaped(run->commands.send_typed(line, (folder / "wave").string()));
      return {};
    };
    if (!run->views) {
      rows.row(here_too("00 " + started, folder));
    } else {
      rows.row(here_too(std::format("00 {}; {}", started, rows.quiet(frame)), folder));
      std::size_t step = 0;
      for (const std::string_view line : lines) {
        const std::string said = rows.quiet([&] {
          std::string done;
          try {
            done = act(line);
          } catch (...) {
            done = " throws " + thrown();
          }
          std::string after;
          try {
            after = frame();
          } catch (...) {
            after = "the frame throws " + thrown();
          }
          // A file's text shows quoted, as it may span lines.
          const std::size_t space = line.find(' ', 7);
          const std::string shown =
              line.starts_with("#write ") && space != std::string_view::npos
                  ? std::format(
                        "{} {}", line.substr(0, space), escaped(line.substr(space + 1)))
                  : std::string(line);
          return std::format("{:02} {}{}; {}", ++step, shown, done, after);
        });
        rows.row(here_too(said, folder));
      }
    }
    rows.quiet([&] {
      run.reset();
      stage.reset();
      return 0;
    });
  });
}

} // namespace

void cases(Bench &bench) {
  bench.check("Views.cpp/edits, undo and redo", [](Rows &rows) {
    session(rows,
            {"node add a",
             "node add b x=1",
             "undo",
             "undo",
             "redo",
             "redo",
             "redo",
             "node remove keep b",
             "undo",
             "view list"});
  });
  // A line that moved where lines go, as view close of the current view does, and the
  // projects part's view new: undo puts them back to the view they went to before it,
  // redo where the line left them, so a replay without the line goes on in the same
  // view (V08). view list marks where they go; a terminal puts that view before each
  // line typed, which the session's lines are not.
  bench.check("Views.cpp/a line that moved where lines go, undone", [](Rows &rows) {
    session(rows,
            {"view add tri ../triangle/view.vlp",
             "view switch tri",
             "view close",
             "view list",
             "undo",
             "view list",
             "redo",
             "view list",
             "undo",
             "view list"});
  });
  // Undo of a line that moved no lines leaves them where they go now: a line typed in
  // tri, lines switched back to the view vulpen started with, then undo, and they stay
  // there.
  bench.check("Views.cpp/undo of a line that moved no lines", [](Rows &rows) {
    session(rows,
            {"view add tri ../triangle/view.vlp",
             "view switch tri",
             "tri: node add b",
             "view switch",
             "undo",
             "view list"});
  });
  bench.check("Views.cpp/view save", [](Rows &rows) {
    session(rows,
            {"node add a",
             "view save",
             "undo",
             "view save force",
             "view save extra",
             "quit"});
  });
  bench.check("Views.cpp/a view hosted beside", [](Rows &rows) {
    session(rows,
            {"view add tri ../triangle/view.vlp",
             "view list",
             "tri: node add c",
             "view close tri",
             "view close force tri",
             "view list",
             "view close",
             "view add w3 ../none/view.vlp",
             "view add w4 ../wave/view.vlp"});
  });
  // Connect lines whose name is left out: two ports, three ports the first of which the
  // running view lists as written (the grammar's name left out), a name and two ports,
  // and words naming a node with no port, which the refusal suggests one for.
  bench.check("Views.cpp/connect lines, a name left out", [](Rows &rows) {
    session(rows,
            {"connect wave.values probe.values",
             "connect wave.values probe.values probe.values",
             "connect again wave.values probe.values",
             "connect wave probe.values",
             "connect wave.values probe",
             "connect probe.values wave.values wave.values"});
  });
  // Param set and connect lines the running bind refuses as they run (Schedule::check):
  // a param the node reads none of, one of the wrong type, ends naming no port, and a
  // port its node reads where a connection's writer goes; then lines it takes.
  bench.check("Views.cpp/param set and connect lines the bind checks", [](Rows &rows) {
    session(rows,
            {"param set wave nothere 1",
             "param set wave amplitude abc",
             "param set probe step 1.5",
             "param set probe step 64",
             "connect x probe.samples wave.nothere",
             "connect y probe.nothere probe.samples",
             "connect z wave.speed probe.samples",
             "param set wave amplitude 2"});
  });
  bench.check("Views.cpp/view show and image clear", [](Rows &rows) {
    // A line names the view it goes to; a front end names the current one for it.
    session(rows,
            {"view add tri ../triangle/view.vlp",
             "view show",
             "tri: view show",
             "tri: view show triangle.pixel",
             "tri: view show nothing.here",
             "tri: view show",
             "image clear wave.values",
             "image clear probe.nothing"});
  });
  // What a forced close or quit force puts back of a project's files, as kept at its
  // hosting: each file changed, made or deleted since, and a file too large to keep then,
  // past kept_most, which then shrank. No GPU: the functions a session reaches them by.
  bench.check("Views.cpp/put_back, a file too large to keep", [](Rows &rows) {
    SandboxFiles files;
    const std::filesystem::path folder =
        planted(rows, {"view.vlp=version = 1", "a=1", "n/b=2", ".vulpen/c=3", "gone=4"});
    Files::save(folder / "large", std::string(kept_most + 1, 'x'));
    View view;
    view.file = folder / "view.vlp";
    const Kept kept = kept_of(files, view);
    rows.row("kept: " + kept_text(kept));
    Files::save(folder / "a", "changed");
    Files::save(folder / "made", "new");
    Files::save(folder / ".vulpen" / "c", "rewritten");
    Files::remove(folder / "gone");
    Files::save(folder / "large", "small now");
    rows.row(
        std::format("changed a, made made, removed gone, large shrank: differences [{}]",
                    joined(differences(kept, kept_of(files, view)), ", ")));
    put_back(files, folder, kept);
    rows.row("put back: " + here(tree_of(folder), folder));
  });
  // A project holding a link out of it, which bench.py plants, as no bench file makes a
  // link (RA04): what is kept of it, and what a forced close leaves of the folder the
  // link leads to, whose files are none of the project's (9i).
  bench.check("Views.cpp/put_back, a link out of the project", [](Rows &rows) {
    const std::filesystem::path linked = rows.root() / "fixtures" / "linked";
    std::error_code missing;
    if (!std::filesystem::is_symlink(linked / "project" / "out", missing)) {
      rows.row("no fixture: bench.py plants <root>/fixtures/linked/ where a link can be "
               "made");
      return;
    }
    SandboxFiles files;
    const std::filesystem::path project = linked / "project";
    const std::filesystem::path outside = linked / "outside";
    View view;
    view.file = project / "view.vlp";
    try {
      const Kept kept = kept_of(files, view);
      rows.row("kept: " + kept_text(kept));
      Files::save(outside / "x", "changed outside");
      Files::save(outside / "made", "made outside");
      rows.row(std::format("outside: x changed, made made: differences [{}]",
                           joined(differences(kept, kept_of(files, view)), ", ")));
      put_back(files, project, kept);
      rows.row("put back, the folder the link leads to: " +
               here(tree_of(outside), linked));
    } catch (...) {
      rows.row("throws " + here(thrown(), linked));
    }
    // As bench.py planted it, for a second pass in the process.
    if (std::filesystem::exists(outside / "made", missing))
      Files::remove(outside / "made");
    Files::save(outside / "x", "outside");
  });
  bench.leaves_out("Views.cpp/put_back, a link out of the project",
                   "threads",
                   "bench.py plants its one fixture in the root, above the folder "
                   "each of two threads at once gets");
  // A view beside one taking its window, as the ide presents a project. The triangle is
  // hosted as raster, the name of the node of triangle-compute whose canvas its screen
  // shows, so a connection from a node takes no view's window by its name alone. Then
  // the screen shows raster's window instead; raster rests while that goes unread, and
  // so does a view it hosts; and the view that presented it keeps the window once it
  // no longer does, until it closes.
  bench.check("Views.cpp/frames, a view beside taking the window", [](Rows &rows) {
    framed(rows,
           {},
           {"view add raster ../triangle/view.vlp",
            "view add tc ../triangle-compute/view.vlp",
            "tc: view show raster.canvas",
            "tc: view show",
            "tc: disconnect canvas",
            "tc: connect show raster: screen.picture",
            "#unread tc/screen/picture",
            "raster: view add inner ../triangle-points/view.vlp",
            "#read tc/screen/picture",
            "tc: disconnect show",
            "view close force tc"});
  });
  // Views closed, and where the lines that went to one go then. A view beside one that
  // goes keeps its connections from others. An app's view, which a plain view save
  // leaves; two apps of one recipes' folder share the name it builds by. The current view
  // closed, its lines go on to the project hosted before it, past an app.
  bench.check("Views.cpp/frames, views closed and where lines go", [](Rows &rows) {
    framed(rows,
           {"recipes/vulpen/library",
            "recipes/custom/app/view.vlp=[manifest]\nversion = 1\n",
            "recipes/custom/app2/view.vlp=[manifest]\nversion = 1\n"},
           {"view add raster ../triangle/view.vlp",
            "view add app ../recipes/custom/app/view.vlp",
            "view add tc ../triangle-compute/view.vlp",
            "tc: disconnect canvas",
            "tc: connect show raster: screen.picture",
            "view add w3 ../none/view.vlp",
            "view close force w3",
            "app: view save",
            "view add app2 ../recipes/custom/app2/view.vlp",
            "view switch tc",
            "view close force tc",
            "view list",
            "view close nothere"});
  });
  // A live build's reload: the view the build tree mirrors by the name given, its
  // manifest read again once an editor changed it, with the edits since it last held the
  // view over it; a manifest that does not load leaves the running graph; one as read
  // rebuilds as it is, taking what the build made.
  bench.check("Views.cpp/frames, a reload after a live build", [](Rows &rows) {
    framed(rows,
           {},
           {"view add raster ../triangle/view.vlp",
            "raster: node add a",
            "#write triangle/view.vlp [manifest]\nversion = 1\n\n[node \"triangle\"]\n"
            "vertex_count = 3\nparam = frames_per_turn=600\n\n[node \"extra\"]\n",
            "#reload triangle",
            "#write triangle/view.vlp [node",
            "#reload triangle",
            "#reload wave"});
  });
  // An image shown in a view's window's place runs only the nodes it needs, back along
  // the connections: ray-cubes' tone, which the rays' dispatches before it feed; the
  // fail-loud view's paint, a draw another node samples; and an image its C++ fills,
  // cleared.
  bench.check("Views.cpp/frames, images shown and cleared", [](Rows &rows) {
    framed(rows,
           {},
           {"view add rc ../ray-cubes/view.vlp",
            "rc: view show tone.picture",
            "rc: view show",
            "view add m ../mistakes/view.vlp",
            "m: view show paint.color",
            "m: image clear picture.picture",
            "m: view show"});
  });
  for (const char *const id : {"Views.cpp/edits, undo and redo",
                               "Views.cpp/view save",
                               "Views.cpp/a view hosted beside",
                               "Views.cpp/view show and image clear",
                               "Views.cpp/connect lines, a name left out",
                               "Views.cpp/param set and connect lines the bind checks",
                               "Views.cpp/frames, a view beside taking the window",
                               "Views.cpp/frames, views closed and where lines go",
                               "Views.cpp/frames, a reload after a live build",
                               "Views.cpp/frames, images shown and cleared"}) {
    carries_layer(bench, id);
    takes_the_main_thread(bench, id);
    bench.leaves_out(id,
                     "odd",
                     "the odd build lays its containers out otherwise "
                     "(_GLIBCXX_DEBUG), so the debug build's node modules do not "
                     "load into it");
  }
}

} // namespace VP::bench
