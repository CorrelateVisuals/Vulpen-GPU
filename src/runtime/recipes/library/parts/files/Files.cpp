#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view manifest = "view.vlp";
// The folders at a view's top that hold what its nodes share (RV05, RV09), so neither is
// a node's, nor any folder in them.
constexpr std::array<std::string_view, 2> shared_folders{"contracts", "shared"};
// What new starts a file with, by its extension: the start file of that extension in the
// start folder, the least that builds and runs, Name in it becoming the class the file's
// stem names; and the word its node needs to run it, which the node gets where it has
// none: the three corners of the start's triangle, and one workgroup of its threads. A
// file of any other extension starts empty.
struct Start {
  std::string_view extension;
  std::string_view word; // as node set takes it; empty for none
};
constexpr std::array<Start, 5> starts{{{".vert", "vertex_count=3"},
                                       {".frag", ""},
                                       {".comp", "invocations=64"},
                                       {".cpp", ""},
                                       {".h", ""}}};
constexpr std::string_view start_name = "Name";

// A path as the prompt shows a folder, from where vulpen started, so an answer names it
// as the person typed it.
std::string shown(const std::filesystem::path &path) {
  return path.lexically_proximate(std::filesystem::current_path()).generic_string();
}

// Without the / that may end a folder's path, so its name is its last part.
std::filesystem::path bare(const std::filesystem::path &path) {
  const std::filesystem::path normal = path.lexically_normal();
  return normal.filename().empty() ? normal.parent_path() : normal;
}

// What its folder lists of a path: a file's name, a folder's with a / after it.
bool listed(VP::FilePort &files, const std::filesystem::path &path, std::string_view end) {
  const std::vector<std::string> names = files.list(path.parent_path().string());
  return std::ranges::find(names, path.filename().string() + std::string(end)) !=
         names.end();
}

bool there(VP::FilePort &files, const std::filesystem::path &path) {
  return listed(files, path, "") || listed(files, path, "/");
}

// The C++ class a name gives: command-line gives CommandLine.
std::string class_of(std::string_view name) {
  std::string type;
  bool upper = true;
  for (const char letter : name) {
    if (letter == '-') {
      upper = true;
      continue;
    }
    type.push_back(upper && letter >= 'a' && letter <= 'z'
                       ? static_cast<char>(letter - 'a' + 'A')
                       : letter);
    upper = false;
  }
  return type;
}

bool word_letter(char letter) {
  return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
         (letter >= '0' && letter <= '9') || letter == '_';
}

// Each whole word `from` in the text, whether code or a comment's, becomes `to`.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
  for (std::size_t at = text.find(from); at != std::string::npos;
       at = text.find(from, at)) {
    const std::size_t end = at + from.size();
    if ((at != 0 && word_letter(text[at - 1])) ||
        (end != text.size() && word_letter(text[end]))) {
      at = end;
      continue;
    }
    text.replace(at, from.size(), to);
    at += to.size();
  }
  return text;
}

// The class a start names: its file's stem as a C++ class, Clock of Clock.cpp and MyNode
// of my-node.cpp; a stem no class can be named by is refused before a file is written.
std::string class_named(const std::filesystem::path &file) {
  const std::string stem = file.stem().string();
  const char first = stem.empty() ? '\0' : stem.front();
  const bool letter = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z');
  if (!VP::is_name(stem) || !letter)
    throw std::runtime_error(std::format("{} names no C++ class: its name before the "
                                         "extension is letters, digits, _ and -, a "
                                         "letter first",
                                         shown(file)));
  return class_of(stem);
}

// Where a path lies in a view vulpen hosts: the name lines address the view with, the
// view as the edits so far left it, and the path from its folder, empty for the folder
// itself.
struct Held {
  std::string name;
  const VP::View *view = nullptr;
  std::filesystem::path inside;
};

// A view vulpen hosts: the name lines address it with, empty for the view vulpen started
// with, the view, and its folder.
struct Hosted {
  std::string name;
  const VP::View *view = nullptr;
  std::filesystem::path folder;
};

// From the view vulpen started with down the views each hosts.
std::vector<Hosted> hosted_views(VP::Call &call) {
  std::vector<Hosted> views;
  std::vector<std::string> names{std::string()};
  for (std::size_t at = 0; at < names.size(); ++at) {
    const VP::View *const view = call.view(names[at]);
    if (!view)
      continue;
    for (const VP::Child &child : view->children)
      names.push_back(child.name);
    views.push_back(
        {names[at],
         view,
         std::filesystem::absolute(view->file.parent_path()).lexically_normal()});
  }
  return views;
}

// The hosted view whose folder holds the path nearest; none for a path no hosted view's
// folder holds, which is a folder like any other, as a project not loaded is.
std::optional<Held> held_by(VP::Call &call, const std::filesystem::path &path) {
  std::optional<Held> nearest;
  for (const Hosted &hosted : hosted_views(call)) {
    std::filesystem::path inside = path.lexically_relative(hosted.folder);
    if (inside.empty() || *inside.begin() == "..")
      continue;
    if (inside == ".")
      inside.clear();
    if (!nearest || std::distance(inside.begin(), inside.end()) <
                        std::distance(nearest->inside.begin(), nearest->inside.end()))
      nearest = Held{hosted.name, hosted.view, inside};
  }
  return nearest;
}

// The node whose folder a folder of a view is, by its path from the view's folder: ui/panel
// is ui.panel's (RV08); none for the shared folders, the hidden ones, as .vulpen, and
// what they hold, where no node is.
std::optional<std::string> node_of(const std::filesystem::path &inside) {
  std::string name;
  for (const std::filesystem::path &part : inside) {
    const std::string word = part.string();
    if (word.starts_with('.') ||
        (name.empty() && std::ranges::find(shared_folders, word) != shared_folders.end()))
      return std::nullopt;
    name += (name.empty() ? "" : ".") + word;
  }
  return name;
}

bool named(const VP::View &view, std::string_view node) {
  return std::ranges::find(view.nodes, node, &VP::Node::name) != view.nodes.end();
}

// Each folder a folder holds, at any depth, as the path at holds it once brought there,
// parents first.
template <class Add>
void each_below(VP::FilePort &files,
                const std::filesystem::path &from,
                const std::filesystem::path &at,
                const Add &add) {
  for (const std::string &name : files.list(from.string()))
    if (name.ends_with('/')) {
      add(at / bare(name));
      each_below(files, from / bare(name), at / bare(name), add);
    }
}

// Every folder in a view is a node's (RV08), so the folders a line makes there are nodes
// the line adds: each folder on the way to the path no node names, and, for a folder
// brought there, the path itself and every folder below it, as a copy or a move brings
// them, parents first. A folder whose name no node's could be is refused before anything
// is written (A02).
std::vector<std::string> nodes_for(VP::FilePort &files,
                                   const Held &held,
                                   bool folder,
                                   const std::optional<std::filesystem::path> &brought) {
  std::vector<std::string> nodes;
  const auto add = [&](const std::filesystem::path &inside) {
    const std::optional<std::string> node = node_of(inside);
    if (!node || named(*held.view, *node) || std::ranges::find(nodes, *node) != nodes.end())
      return;
    if (!VP::is_name(inside.filename().string()))
      throw std::runtime_error(std::format(
          "{}/ in view {} would be node {}'s folder, as every folder in a view is a node's "
          "(RV08), but a node's name holds letters, digits, _ and -",
          inside.generic_string(),
          held.view->name,
          *node));
    nodes.push_back(*node);
  };
  std::filesystem::path inside;
  for (const std::filesystem::path &part : held.inside) {
    inside /= part;
    if (inside == held.inside && !folder)
      break;
    add(inside);
  }
  if (folder && brought)
    each_below(files, *brought, held.inside, add);
  return nodes;
}

// A folder holding a hosted view's folder, as projects/ holds every project's, moves and
// goes only once that view is let go, as the view's own folder does.
void check_holds(VP::Call &call, const std::filesystem::path &path) {
  for (const Hosted &hosted : hosted_views(call)) {
    const std::filesystem::path inside = hosted.folder.lexically_relative(path);
    if (inside.empty() || inside == "." || *inside.begin() == "..")
      continue;
    throw std::runtime_error(
        hosted.name.empty()
            ? std::format("{}/ holds {}/, the folder of the view vulpen started with, "
                          "which stays while vulpen runs",
                          shown(path),
                          shown(hosted.folder))
            : std::format("{}/ holds {}/, view {}'s folder, which stays while the view "
                          "runs: view close {} lets it go",
                          shown(path),
                          shown(hosted.folder),
                          hosted.name,
                          hosted.name));
  }
}

// A view's folder and manifest move and go only with the lines that keep a view and its
// folders together, and a node's folder goes only with its node, which the refusal names
// (A02); a rename of a node's folder renames its node.
void check_kept(VP::Call &call,
                const std::optional<Held> &held,
                const std::filesystem::path &path,
                bool renaming) {
  check_holds(call, path);
  if (!held)
    return;
  if (held->inside.empty() || held->inside == manifest)
    throw std::runtime_error(std::format(
        "{} is view {}'s {}, which {}",
        shown(path),
        held->view->name,
        held->inside.empty() ? "folder" : "manifest",
        renaming ? std::format("{}: view rename <name> moves", held->view->name)
                 : std::format("goes once view close {} lets the view go",
                               held->view->name)));
  if (const std::optional<std::string> node = node_of(held->inside);
      !renaming && node && named(*held->view, *node))
    throw std::runtime_error(std::format(
        "{}/ is node {}'s folder, which node remove {} deletes with the node",
        shown(path),
        *node,
        *node));
}

// The node whose folder a path in a view is; none for any other path.
std::optional<std::string> node_at(const std::optional<Held> &held) {
  if (!held || held->inside.empty())
    return std::nullopt;
  std::optional<std::string> node = node_of(held->inside);
  return node && named(*held->view, *node) ? node : std::nullopt;
}

// A command's words carry no blanks, so a value holding one cannot be sent.
std::string sendable(std::string_view key, std::string_view value) {
  if (value.find_first_of(" \t") != std::string_view::npos)
    throw std::runtime_error(std::format(
        "{} {} holds a blank, which a command's word cannot carry", key, value));
  return std::format(" {}={}", key, value);
}

// The graph's side of a node's rename, as the edits a person would type, so a replay
// renames it alike (V08): the connections reaching the node or the nodes inside it out,
// the node out, and each of them back under the new name with the words it had, in the
// order they were, back where the node was among the others, and the connections back,
// those named inside it renamed too.
std::vector<std::string>
renames(const VP::View &view, const std::string &from, const std::string &to) {
  const auto moved = [&](std::string_view name) -> std::optional<std::string> {
    if (!VP::within(name, from))
      return std::nullopt;
    return std::format("{}{}", to, name.substr(from.size()));
  };
  const auto end = [&](VP::Endpoint endpoint) {
    if (!endpoint.view())
      endpoint.node = moved(endpoint.node).value_or(endpoint.node);
    return endpoint.text();
  };
  std::vector<std::string> lines;
  std::vector<const VP::Connection *> reaching;
  for (const VP::Connection &connection : view.connections) {
    const auto inside = [&](const VP::Endpoint &endpoint) {
      return !endpoint.view() && moved(endpoint.node);
    };
    if (inside(connection.from) || std::ranges::any_of(connection.to, inside)) {
      reaching.push_back(&connection);
      lines.push_back(std::format("disconnect {}", connection.name));
    }
  }
  lines.push_back(std::format("node remove keep {}", from));
  std::optional<std::string> after;
  bool past = false;
  for (const VP::Node &node : view.nodes) {
    if (const std::optional<std::string> name = moved(node.name)) {
      past = true;
      std::string line = std::format("node add {}", *name);
      for (const VP::Word &word : VP::words_of(node))
        line += sendable(word.key, word.value);
      lines.push_back(std::move(line));
    } else if (past && !after) {
      after = node.name;
    }
  }
  if (after)
    lines.push_back(std::format("node move {} {}", to, *after));
  // Only a name inside the node's is the node's, as a drop names a connection inside the
  // recipe; one named as the node is, as corners carrying corners, keeps its name.
  for (const VP::Connection *const connection : reaching) {
    const std::string &name = connection->name;
    std::string line = std::format("connect {} {}",
                                   name == from ? name : moved(name).value_or(name),
                                   end(connection->from));
    for (const VP::Endpoint &reader : connection->to)
      line += ' ' + end(reader);
    lines.push_back(std::move(line));
  }
  return lines;
}

// A node and the words a line gives it, each after a blank, as node add and node set
// take them.
struct Words {
  std::string node;
  std::string words;
};

// What a line changes in a view's graph, known before anything is written: the nodes it
// adds, and the words it sets on nodes the view has.
struct Graphed {
  std::string view; // the name lines address it with
  std::vector<Words> adds;
  std::vector<Words> sets;
  std::vector<std::string> then; // lines after those, before the save, as a rename's
};

Graphed &graphed(std::vector<Graphed> &graphs, const std::string &view) {
  const auto found = std::ranges::find(graphs, view, &Graphed::view);
  return found != graphs.end() ? *found : graphs.emplace_back(Graphed{.view = view});
}

// The word a start needs, given to a node as the line adds it, or set on the view's node,
// while the node counts nothing yet, by the view's words or the line's: a node counts its
// vertices or its invocations, never both, so a start that asks for the other leaves it,
// for the bind to name (A02).
void give(Graphed &graph,
          const VP::View &view,
          const std::string &node,
          std::string_view word) {
  std::string words; // the node's, as the view and the line give them
  if (const auto had = std::ranges::find(view.nodes, node, &VP::Node::name);
      had != view.nodes.end())
    for (const VP::Word &given : VP::words_of(*had))
      words.append(std::format(" {}={}", given.key, given.value));
  const auto added = std::ranges::find(graph.adds, node, &Words::node);
  if (added != graph.adds.end())
    words.append(added->words);
  for (const Words &set : graph.sets)
    if (set.node == node)
      words.append(set.words);
  const bool counts = std::ranges::any_of(starts, [&](const Start &start) {
    const std::string_view key = start.word.substr(0, start.word.find('='));
    return !key.empty() && words.find(std::format(" {}=", key)) != std::string::npos;
  });
  if (counts)
    return;
  if (added != graph.adds.end())
    added->words.append(" ").append(word);
  else
    graph.sets.push_back({node, std::format(" {}", word)});
}

// Files and folders made, renamed, copied and deleted through the file port (V05), so a
// person does all of it from inside vulpen, by lines a GUI sends too (V06). A path typed
// counts from the folder the prompt shows, as in a shell, and one in a script from beside
// it. In a view vulpen hosts every folder is a node's (RV08): a folder a line makes or
// brings there is a node, which the line adds, saving the view, so new makes nodes as it
// makes their folders, and a shader or C++ file it makes starts as the least code that
// builds and runs; a line that would move or delete a node's folder or the view's own is
// refused, naming the line that keeps them together (A02), and so is a folder in an app,
// whose nodes use the library's recipes as they are. Nothing is written over: a line
// whose path is taken is refused, and a folder holding files goes only with force, as
// quit force ends a run that would lose work.
class Files final : public VP::Operator {
  void bind(VP::Bind &node) override {
    // A part's folder is <recipes>/library/parts/<name>; the apps are beside the library.
    _recipes = std::filesystem::path(node.folder()).parent_path().parent_path().parent_path();
    for (std::size_t start = 0; start < starts.size(); ++start)
      _starts[start] =
          node.file(std::format("start/{}{}.in", start_name, starts[start].extension));
    _new = node.command("new <file>...",
                        "makes files and folders, and the folders they are in: a path "
                        "ending in / or with no extension a folder, else a file, a "
                        "shader's or C++'s with the least code that builds and runs; in a "
                        "view a folder is a node, which it adds, a draw's with its "
                        "vertex_count and a dispatch's with its invocations, saving the "
                        "view");
    _rename = node.command("rename <file> <file>",
                           "renames a file or a folder, or moves it, into folders made "
                           "for it, in a view nodes; a node's folder renames the node, "
                           "the nodes inside it and the connections naming them, inside "
                           "another node's folder inside that node; a view's folder "
                           "moves with view rename");
    _copy = node.command("file copy <file> <file>",
                         "copies a file, or a folder with all it holds, whole or not at "
                         "all; in a view each folder it brings is a node, which it adds");
    _remove = node.command("file remove <file>",
                           "deletes a file or an empty folder; a node's folder goes with "
                           "node remove, a view's once view close let it go");
    _remove_force = node.command("file remove force <file>",
                                 "deletes a folder with all it holds, as file remove does "
                                 "the rest");
  }

  void command(VP::Call &call) override {
    const std::span<const std::string_view> words = call.arguments();
    if (call.is(_new))
      make(call);
    else if (call.is(_rename) || call.is(_copy))
      move(call, bare(words[0]), bare(words[1]), call.is(_copy));
    else if (call.is(_remove) || call.is(_remove_force))
      remove(call, bare(words[0]), call.is(_remove_force));
  }

  // Each path, all checked before one is made, so a line refused makes nothing: a folder
  // for one ending in / or with no extension, as a node's name has none, else a file, as
  // its start has it. In a view each folder made is a node the line adds, a node a start
  // needs a word of gets it where it has none, and each view is saved once.
  void make(VP::Call &call) const {
    std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> made;
    std::vector<Graphed> graphs;
    for (const std::string_view word : call.arguments()) {
      const std::filesystem::path path = bare(word);
      const bool folder =
          std::filesystem::path(word).filename().empty() || !path.has_extension();
      const bool twice =
          std::ranges::any_of(made, [&](const auto &one) { return one.first == path; });
      if (twice || there(call.files(), path))
        throw std::runtime_error(std::format("{} is there already", shown(path)));
      const std::optional<Held> held = held_by(call, path);
      Graphed *const graph = held ? &graphed(graphs, held->name) : nullptr;
      for (std::string &node : adopted(call, held, folder, std::nullopt))
        if (std::ranges::find(graph->adds, node, &Words::node) == graph->adds.end())
          graph->adds.push_back({std::move(node), {}});
      made.emplace_back(path,
                        folder ? std::nullopt
                               : std::optional(started(call.files(), path, held, graph)));
    }
    std::string names;
    for (const auto &[path, text] : made) {
      if (text)
        call.files().save(path.string(), *text);
      else
        call.files().make(path.string());
      names.append(names.empty() ? "" : ", ").append(shown(path)).append(text ? "" : "/");
    }
    call.reply(std::format("made {}{}", names, edit(call, graphs)));
  }

  // A file's start, by its extension, its class named by the file's stem where the start
  // names one, and the word it needs given to the node whose folder holds the file.
  std::string started(VP::FilePort &files,
                      const std::filesystem::path &file,
                      const std::optional<Held> &held,
                      Graphed *graph) const {
    const auto start =
        std::ranges::find(starts, file.extension().string(), &Start::extension);
    if (start == starts.end())
      return {};
    const auto at = static_cast<std::size_t>(start - starts.begin());
    std::string text(files.text(_starts[at]));
    if (text.find(start_name) != std::string::npos)
      text = replaced(std::move(text), start_name, class_named(file));
    const std::optional<std::string> node =
        held ? node_of(held->inside.parent_path()) : std::nullopt;
    if (node && !node->empty() && !start->word.empty())
      give(*graph, *held->view, *node, start->word);
    return text;
  }

  // A rename takes what it names along, so a node's folder or a view's own stays where
  // its name puts it; a copy leaves it, so only where the copy lands counts. Both name
  // the paths as typed, as new and file remove do.
  void move(VP::Call &call,
            const std::filesystem::path &from,
            const std::filesystem::path &to,
            bool copy) const {
    if (!there(call.files(), from))
      throw std::runtime_error(std::format("{} is not there", shown(from)));
    if (there(call.files(), to))
      throw std::runtime_error(std::format("{} is there already", shown(to)));
    const std::optional<Held> left = held_by(call, from);
    if (!copy) {
      check_kept(call, left, from, true);
      if (const std::optional<std::string> node = node_at(left))
        return rename_node(call, *left, *node, from, to);
    }
    const std::optional<Held> held = held_by(call, to);
    std::vector<Graphed> graphs;
    for (std::string &node : adopted(call, held, listed(call.files(), from, "/"), from))
      graphed(graphs, held->name).adds.push_back({std::move(node), {}});
    if (copy)
      call.files().copy(from.string(), to.string());
    else
      call.files().rename(from.string(), to.string());
    call.reply(std::format("{} {} to {}{}",
                           copy ? "copied" : "renamed",
                           shown(from),
                           shown(to),
                           edit(call, graphs)));
  }

  // A node's folder renamed renames the node, the nodes inside it and the connections
  // naming them, its folder going where the new path is, inside another node's folder
  // inside that node, and folders on the way no node names becoming nodes. It stays in
  // its view, whose edits these are; copy brings a node into another. The edits are
  // known, and the new name checked, before the folder moves.
  void rename_node(VP::Call &call,
                   const Held &held,
                   const std::string &node,
                   const std::filesystem::path &from,
                   const std::filesystem::path &to) const {
    const std::optional<Held> into = held_by(call, to);
    if (!into || into->name != held.name)
      throw std::runtime_error(std::format("{}/ is node {}'s folder, which a rename moves "
                                           "within view {}; copy brings it into another",
                                           shown(from),
                                           node,
                                           held.view->name));
    const std::optional<std::string> name = node_of(into->inside);
    const bool nameable =
        std::ranges::all_of(into->inside, [](const std::filesystem::path &part) {
          return VP::is_name(part.string());
        });
    if (!name || name->empty() || !nameable)
      throw std::runtime_error(std::format(
          "{}/ would be no node's folder, as node {}'s must be: a node's name holds "
          "letters, digits, _ and -, and a shared or hidden folder holds no node",
          shown(to),
          node));
    if (VP::within(*name, node))
      throw std::runtime_error(std::format("{} would be inside {} itself", *name, node));
    std::vector<Graphed> graphs;
    Graphed &graph = graphed(graphs, held.name);
    for (std::string &way : adopted(call, into, false, std::nullopt))
      graph.adds.push_back({std::move(way), {}});
    graph.then = renames(*held.view, node, *name);
    call.files().rename(from.string(), to.string());
    call.reply(std::format("renamed {}/ to {}/, node {} to {}{}",
                           shown(from),
                           shown(to),
                           node,
                           *name,
                           edit(call, graphs)));
  }

  // The nodes the folders a line makes or brings into a view are, known before anything is
  // written; in an app, which has no folders of its own, none may be.
  std::vector<std::string> adopted(VP::Call &call,
                                   const std::optional<Held> &held,
                                   bool folder,
                                   const std::optional<std::filesystem::path> &brought) const {
    if (!held)
      return {};
    std::vector<std::string> nodes = nodes_for(call.files(), *held, folder, brought);
    const std::filesystem::path inside =
        std::filesystem::absolute(held->view->file.parent_path())
            .lexically_normal()
            .lexically_relative(_recipes);
    if (!nodes.empty() && !inside.empty() && *inside.begin() != "..")
      throw std::runtime_error(std::format(
          "view {} is an app, whose nodes use the library's recipes as they are, so it "
          "holds no folder of its own, as node {} would; a project holds them",
          held->view->name,
          nodes.front()));
    return nodes;
  }

  // Each view's nodes added with their words, the words set, the lines after them sent,
  // and the view saved, so its manifest holds every node whose folder is there; what the
  // answer adds, as ", added node cube vertex_count=3 and saved the view".
  static std::string edit(VP::Call &call, std::span<const Graphed> graphs) {
    std::string added;
    std::string given;
    std::size_t count = 0;
    for (const Graphed &graph : graphs) {
      for (const Words &add : graph.adds) {
        call.commands().send(
            std::format("{}: node add {}{}", graph.view, add.node, add.words));
        added.append(added.empty() ? "" : ", ").append(add.node).append(add.words);
        ++count;
      }
      for (const Words &set : graph.sets) {
        call.commands().send(
            std::format("{}: node set {}{}", graph.view, set.node, set.words));
        given.append(given.empty() ? "" : ", ").append(set.node).append(set.words);
      }
      for (const std::string &line : graph.then)
        call.commands().send(std::format("{}: {}", graph.view, line));
      if (!graph.adds.empty() || !graph.sets.empty() || !graph.then.empty())
        call.commands().send(std::format("{}: view save", graph.view));
    }
    if (std::ranges::all_of(graphs, [](const Graphed &graph) {
          return graph.adds.empty() && graph.sets.empty() && graph.then.empty();
        }))
      return {};
    return std::format("{}{} and saved the view",
                       added.empty() ? ""
                                     : std::format(", added node{} {}",
                                                   count == 1 ? "" : "s",
                                                   added),
                       given.empty() ? "" : std::format(", gave node {}", given));
  }

  static void remove(VP::Call &call, const std::filesystem::path &path, bool force) {
    VP::FilePort &files = call.files();
    if (!there(files, path))
      throw std::runtime_error(std::format("{} is not there", shown(path)));
    check_kept(call, held_by(call, path), path, false);
    if (!listed(files, path, "/")) {
      files.remove(path.string());
      return call.reply(std::format("removed {}", shown(path)));
    }
    const std::size_t held = count(files, path);
    const std::string some = std::format("{} file{}", held, held == 1 ? "" : "s");
    if (held > 0 && !force)
      throw std::runtime_error(std::format("{}/ holds {}, which a delete cannot bring "
                                           "back: file remove force {}/ deletes them",
                                           shown(path),
                                           some,
                                           shown(path)));
    remove_all(files, path);
    call.reply(std::format("removed {}/{}", shown(path), held == 0 ? "" : " and " + some));
  }

  // The files a folder holds, and those the folders in it hold.
  static std::size_t count(VP::FilePort &files, const std::filesystem::path &folder) {
    std::size_t held = 0;
    for (const std::string &name : files.list(folder.string()))
      held += name.ends_with('/') ? count(files, folder / bare(name)) : 1;
    return held;
  }

  // Deepest first, as the port takes a folder its last file leaves empty, then the folder
  // itself, unless that took it already.
  static void remove_all(VP::FilePort &files, const std::filesystem::path &folder) {
    for (const std::string &name : files.list(folder.string()))
      if (name.ends_with('/'))
        remove_all(files, folder / bare(name));
      else
        files.remove((folder / name).string());
    if (there(files, folder))
      files.remove(folder.string());
  }

  std::filesystem::path _recipes; // the library's folder and the apps' beside it
  // The start files, by starts, which the file port watches, so new writes them as they
  // are now.
  std::array<VP::File, starts.size()> _starts;
  VP::Command _new;
  VP::Command _rename;
  VP::Command _copy;
  VP::Command _remove;
  VP::Command _remove_force;
};

} // namespace

VP_OPERATOR(Files);
