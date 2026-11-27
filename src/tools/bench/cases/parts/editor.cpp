#include "editor/Editor.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/editor: open hands a file or a folder to whichever takes it, its
// one owner (V06): a folder to the graph while the view its folders param names runs, an
// image to the preview while its images param's runs, a file to the editor in the window
// while its window param's runs, else either to the program its program param names. A
// word names a file the view holds, a path any; one that names nothing is refused.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// The node in view p, in a folder holding view.vlp, Main.cpp, pic.PNG, a folder sub, and
// Twice.h in two nodes' folders.
struct Framed {
  Running<Editor> node;
  FakeCommands commands;
  FakeProcesses processes;
  SandboxFiles files;
  std::filesystem::path place;

  Framed(const Rows &rows, Params params) : node(std::move(params)) {
    place = planted(rows,
                    {"view.vlp=[manifest]\n",
                     "main/Main.cpp=int main;\n",
                     "pic.PNG=",
                     "sub",
                     "one/Twice.h=",
                     "two/Twice.h="});
    node.bind.node_name = "edit";
    node.bound();
  }
  // open with these words, the views named running: what it sent, started and answered,
  // or what it refused.
  std::string opened(std::vector<std::string> words,
                     const std::vector<std::string> &running,
                     Rows &rows) {
    FakeCall call;
    call.running = {0};
    call.words = std::move(words);
    call.addressed.file = place / "view.vlp";
    for (const std::string &view : running)
      call.views[view] = View{};
    const auto node_of = [&](std::string name, std::vector<std::string> held) {
      Node made;
      made.name = name;
      made.folder = place / name;
      made.files = std::move(held);
      return made;
    };
    call.unfolded_view.nodes = {node_of("main", {"Main.cpp"}),
                                node_of("one", {"Twice.h"}),
                                node_of("two", {"Twice.h"})};
    call.command_port = &commands;
    call.file_port = &files;
    call.process_port = &processes;
    commands.sent.clear();
    processes.started.clear();
    std::string said;
    try {
      node.called(call);
      said = std::format("sent {}; started {}; answered {}",
                         show(commands.sent, rows.names()),
                         show(processes.started, rows.names()),
                         show(call.replies, rows.names()));
    } catch (...) {
      said = thrown();
    }
    // The paths as a person reads them, from where vulpen started, which the cwd
    // context moves: the folder in either form shows as <here>.
    const std::string shown_folder = shown(place);
    for (std::size_t at = said.find(shown_folder); at != std::string::npos;
         at = said.find(shown_folder, at))
      said.replace(at, shown_folder.size(), "<here>");
    return here(said, place);
  }
};

Params every() {
  return {{"program", "code"},
          {"window", "ide:editor"},
          {"folders", "ide:graph"},
          {"images", "ide:viewer"}};
}

} // namespace

void cases(Bench &bench) {
  bench.check("Editor.cpp/Editor::bind", [](Rows &rows) {
    for (const Params &params : {every(), Params{{"program", "code"}}}) {
      Running<Editor> node(params);
      node.bound();
      rows.row(std::format(
          "{} -> {}", show(params, rows.names()), show(node.bind.asked, rows.names())));
    }
  });

  // With the ide running: a folder to the graph, an image to the preview, a file to the
  // editor in the window, none the view's manifest there; a word the view holds once, a
  // word it holds twice or not at all, and a path that names nothing.
  bench.check("Editor.cpp/Editor::command", [](Rows &rows) {
    Framed frame(rows, every());
    const auto open = [&](std::vector<std::string> words, std::string_view what) {
      rows.row(
          std::format("{} -> {}", what, frame.opened(std::move(words), {"ide"}, rows)));
    };
    open({(frame.place / "sub").string()}, "a folder");
    open({(frame.place / "pic.PNG").string()}, "an image, its ending in capitals");
    open({(frame.place / "main" / "Main.cpp").string()}, "a file");
    open({}, "none");
    open({"Main.cpp"}, "a word the view holds once");
    open({"Twice.h"}, "a word the view holds twice");
    open({"Gone.h"}, "a word the view holds not");
    open({(frame.place / "Gone.h").string()}, "a path that names nothing");
    open({(frame.place / "main" / "Twice.h").string()},
         "a path that names nothing, its name held elsewhere");
    open({(frame.place / "sub" / ".." / "view.vlp").string()}, "a path through ..");
  });

  // With nothing running: either to the program, none the view's folder; a program the
  // PATH lacks refused, naming the param that names another; and with the window alone
  // running, a folder refused, as the editor there opens files.
  bench.check("Editor.cpp/Editor::in_program", [](Rows &rows) {
    Framed frame(rows, every());
    rows.row("a file -> " +
             frame.opened({(frame.place / "main" / "Main.cpp").string()}, {}, rows));
    rows.row("a folder -> " + frame.opened({(frame.place / "sub").string()}, {}, rows));
    rows.row("none -> " + frame.opened({}, {}, rows));
    frame.processes.missing = "code";
    rows.row("code not on the PATH -> " + frame.opened({}, {}, rows));
    Framed window(rows, {{"program", "code"}, {"window", "ide:editor"}});
    rows.row("the window alone running, a folder -> " +
             window.opened({(window.place / "sub").string()}, {"ide"}, rows));
    rows.row("the window alone running, an image -> " +
             window.opened({(window.place / "pic.PNG").string()}, {"ide"}, rows));
  });

  // An image's ending, whatever its case.
  bench.check("Editor.cpp/image", [](Rows &rows) {
    for (const std::string_view path :
         {"a.png", "a.PNG", "a.jpeg", "a.JPG", "a.webp", "a", ".png", "a.png/b"})
      rows.row(std::format("{} -> {}", path, image(path)));
  });

  // Whether a path names a file or a folder, as its folder lists it: a file, a folder, a
  // root, ./ and a name not there.
  bench.check("Editor.cpp/named, folder", [](Rows &rows) {
    const std::filesystem::path place = planted(rows, {"f.txt=", "d"});
    SandboxFiles files;
    for (const std::filesystem::path &path : {place / "f.txt",
                                              place / "d",
                                              place / "d" / "",
                                              place / ".",
                                              place / "gone",
                                              std::filesystem::path("/")})
      rows.row(here(std::format("{} -> named {}, folder {}",
                                path.string(),
                                named(files, path),
                                folder(files, path)),
                    place));
  });
}

} // namespace VP::bench
