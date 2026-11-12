#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Costs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view note_gap = "  "; // between a folder and what it is
constexpr std::size_t bar_room = 10;        // a bar's #, for the costliest beside it
// Half a microsecond, in milliseconds: a time below it prints as 0.000, so it has no bar.
constexpr float least_shown = 0.0005f;
constexpr std::size_t no_node = static_cast<std::size_t>(-1);

// A row ls answers: a folder or a file, indented under the folder it is in, and what
// the folder is, empty for none.
using Rows = std::vector<std::pair<std::string, std::string>>;

// A file's lines, as docs/statistics counts them: one holding any code counts as code, one
// a comment starts as comment, and one of blanks as blank; a file holding a zero byte is
// no text, and has none.
struct Lines {
  std::size_t code = 0;
  std::size_t comment = 0;
  std::size_t blank = 0;
  bool text = true;

  std::size_t all() const {
    return code + comment + blank;
  }
  void add(const Lines &more) {
    code += more.code;
    comment += more.comment;
    blank += more.blank;
  }
};

// What starts a comment, by a file's ending: // in C++ and GLSL, # in a manifest, CMake,
// Python, shell and a settings file; none in any other, as Markdown.
std::string_view comment_of(const std::filesystem::path &file) {
  static constexpr auto slashed = std::to_array<std::string_view>(
      {".cpp", ".h", ".hpp", ".c", ".cc", ".glsl", ".vert", ".frag", ".comp", ".tesc",
       ".tese", ".geom", ".in"});
  static constexpr auto hashed =
      std::to_array<std::string_view>({".vlp", ".py", ".cmake", ".sh", ".ps1", ".ini"});
  const std::string ending = file.extension().string();
  if (std::ranges::find(slashed, ending) != slashed.end())
    return "//";
  if (std::ranges::find(hashed, ending) != hashed.end() || file.filename() == "CMakeLists.txt")
    return "#";
  return {};
}

Lines lines_of(std::string_view text, std::string_view comment) {
  Lines lines;
  if (text.find('\0') != std::string_view::npos) {
    lines.text = false;
    return lines;
  }
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::string_view line = text.substr(at, end - at);
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
      ++lines.blank;
    else if (!comment.empty() && line.substr(first).starts_with(comment))
      ++lines.comment;
    else
      ++lines.code;
    at = end + 1;
  }
  return lines;
}

std::string said(const Lines &lines) {
  return std::format(
      "{}: {} code, {} comment, {} blank", lines.all(), lines.code, lines.comment, lines.blank);
}

// A path as every answer names it, from where vulpen started.
std::string shown(const std::filesystem::path &path) {
  return path.lexically_proximate(std::filesystem::current_path()).generic_string();
}

// What the nodes of a view cost, as probe stats prints them: each node's parent, the
// nodes inside each, the roots last, and what each costs with the nodes inside it.
struct Tree {
  std::vector<std::size_t> parents;
  std::vector<std::vector<std::size_t>> inside;
  std::vector<Weight> sums;
};

// A node's parent is the node whose name its own starts with, up to a dot, the longest
// such, as a view names the nodes inside a node (RV08).
Tree tree_of(std::span<const VP::Cost> costs) {
  Tree tree{.parents = std::vector<std::size_t>(costs.size(), no_node),
            .inside = std::vector<std::vector<std::size_t>>(costs.size() + 1),
            .sums = std::vector<Weight>(costs.size())};
  for (std::size_t at = 0; at < costs.size(); ++at) {
    const std::string_view name = costs[at].node;
    for (std::size_t dot = name.rfind('.');
         dot != std::string_view::npos && tree.parents[at] == no_node;
         dot = dot == 0 ? std::string_view::npos : name.rfind('.', dot - 1))
      for (std::size_t other = 0; other < costs.size(); ++other)
        if (costs[other].node == name.substr(0, dot))
          tree.parents[at] = other;
    const std::size_t parent = tree.parents[at];
    tree.inside[parent == no_node ? costs.size() : parent].push_back(at);
    for (std::size_t up = at; up != no_node; up = tree.parents[up])
      add(tree.sums[up], costs[at]);
  }
  return tree;
}

// A time and a bar of # as long as its share of the most, or blanks for none; ? for a
// pass the GPU did not time.
std::string column(float milliseconds, float most, bool has, bool untimed) {
  if (!has)
    return std::string(8 + bar_room, ' ');
  if (untimed && milliseconds == 0.0f)
    return std::format("{:>7} {:<{}}", "?", "", bar_room);
  const auto bars = milliseconds >= least_shown
                        ? static_cast<std::size_t>(std::lround(
                              static_cast<float>(bar_room) * milliseconds / most))
                        : 0;
  return std::format("{:7.3f} {:<{}}", milliseconds, std::string(bars, '#'), bar_room);
}

// A param of a node of another view, as <view>:<node> names the node, while that view
// runs and the node sets it: its own, or for a node inside a recipe a node uses, the one
// the view keeps for it with the user, as param set keeps it (V11); none else.
std::optional<std::string>
param_there(VP::Call &call, std::string_view target, std::string_view key) {
  const std::size_t colon = target.find(':');
  const VP::View *const view =
      colon == std::string_view::npos ? nullptr : call.view(target.substr(0, colon));
  if (!view)
    return std::nullopt;
  const std::string_view name = target.substr(colon + 1);
  const VP::Node *holder = nullptr;
  std::string wanted(key);
  if (const auto node = std::ranges::find(view->nodes, name, &VP::Node::name);
      node != view->nodes.end())
    holder = &*node;
  for (std::size_t dot = name.find('.'); !holder && dot != std::string_view::npos;
       dot = name.find('.', dot + 1))
    if (const auto user =
            std::ranges::find(view->nodes, name.substr(0, dot), &VP::Node::name);
        user != view->nodes.end() && user->uses()) {
      holder = &*user;
      wanted = std::format("{}.{}", name.substr(dot + 1), key);
    }
  if (!holder)
    return std::nullopt;
  const VP::Param *const param = holder->param(wanted);
  if (!param || param->value.empty())
    return std::nullopt;
  return param->value;
}

// Reads the graph and answers ls and info, so a view can be read without a window and a
// front end needs no mirror of the graph of its own. info with no node asks for the nodes
// selected through selected, the command a part that selects registers, as the graph
// does, so it reads a selection without a connection, which the CLI could not give it.
// ls lists where a graph in a window is, as the terminal there shows it: while the view
// its folders param names runs, the folder its node there shows in place of the view, and
// while the view its graph param names runs, the inside of the node the graph there
// shows, as each node's param says; else the view whole. So ls follows cd, as a shell's
// does, as open finds the graph by a param too.
class Inspect final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _graph = node.sets("graph") ? node.param<std::string>("graph") : std::string();
    _folders = node.sets("folders") ? node.param<std::string>("folders") : std::string();
    _ls = node.command("ls",
                       "lists the view as folders: each node with its files, and each "
                       "view it hosts; or where the graph in a window is, the folder it "
                       "shows or the inside of the node it shows");
    _info = node.command("info [<node>]",
                         "shows a node's words, its folder and the files it holds with "
                         "their lines, or a hosted view's words, and the connections it "
                         "writes and reads; or by its path a file's node and lines, or a "
                         "folder's node and what it holds; with none, each the graph has "
                         "selected under its header");
    _stats = node.command("probe stats [<node>]",
                          "prints what each node costs a frame on the CPU and the GPU, "
                          "in milliseconds smoothed over about a second, as a tree, a "
                          "node holding others counting theirs, each bar to the "
                          "costliest beside it; with a node, that node's");
  }

  void command(VP::Call &call) override {
    if (call.is(_stats))
      stats(call,
            call.arguments().empty() ? std::string_view() : call.arguments().front());
    else if (call.is(_ls))
      here(call);
    else if (call.is(_info) && !call.arguments().empty())
      info(call, call.view(), call.arguments().front());
    else if (call.is(_info))
      info_selected(call);
  }

  // Each as the manifest's section of it would read, header first: a node's, or a view's
  // the view hosts; and a file or a folder selected, which selected names by its path,
  // under one of its own.
  static void info_selected(VP::Call &call) {
    const std::vector<VP::Usage> usages = call.commands().usages();
    if (std::ranges::find(usages, "selected", &VP::Usage::usage) == usages.end())
      throw std::runtime_error("nothing here selects nodes; info <node> shows one");
    const std::string selected = call.commands().send("selected");
    const VP::View &view = call.view();
    bool shown = false;
    for (std::size_t at = 0; at < selected.size();) {
      const std::size_t end = std::min(selected.find('\n', at), selected.size());
      const std::string node = selected.substr(at, end - at);
      at = end + 1;
      const bool hosted =
          std::ranges::find(view.children, node, &VP::Child::name) != view.children.end();
      const bool named =
          std::ranges::find(view.nodes, node, &VP::Node::name) != view.nodes.end();
      if (hosted || named) {
        call.reply(std::format("[{} \"{}\"]", hosted ? "view" : "node", node));
        info(call, view, node);
        shown = true;
      } else if (node.find('/') != std::string::npos) {
        // Named from where vulpen started, as the graph names what it sends.
        shown = info_path(call, view, std::filesystem::absolute(node), true) || shown;
      }
    }
    if (!shown)
      throw std::runtime_error("nothing is selected; info <node> shows a node, and info "
                               "<path> a file or a folder");
  }

  // Where the graph is: the folder it shows, its names one a row, a folder's with a /
  // after it; the inside of the node it shows, its files and the nodes inside it as ls
  // lists them, each recipe unfolded, as the graph shows it; or the view whole.
  void here(VP::Call &call) const {
    if (const std::optional<std::string> folder = param_there(call, _folders, "folder")) {
      for (const std::string &name : call.files().list(*folder))
        call.reply(name);
      return;
    }
    const std::optional<std::string> inside = param_there(call, _graph, "inside");
    if (!inside)
      return ls(call, call.view());
    const VP::View &view = call.unfolded();
    const auto node = std::ranges::find(view.nodes, *inside, &VP::Node::name);
    if (node == view.nodes.end())
      return ls(call, call.view());
    Rows rows;
    for (const std::string &file : node->files)
      rows.emplace_back(file, "");
    const std::string prefix = node->name + '.';
    for (const VP::Node &inner : view.nodes)
      if (inner.name.starts_with(prefix) &&
          inner.name.find('.', prefix.size()) == std::string::npos)
        list(view, inner, 0, rows);
    reply_rows(call, rows);
  }

  // The view as the folders its nodes are (RV08): each node a folder under the node it is
  // inside, its files under it and beside it the recipe it uses or was dropped from, and
  // each view it hosts a folder, beside it the file it is hosted from. Connections are
  // each node's, which info shows. What each folder is reads down one column.
  static void ls(VP::Call &call, const VP::View &view) {
    Rows rows;
    for (const VP::Child &child : view.children)
      rows.emplace_back(child.name + '/',
                        std::format("view, from {}", child.file.string()));
    // The top node's files lie beside the manifest, its folder the view's own.
    for (const VP::Node &node : view.nodes)
      if (VP::is_top(view, node.name))
        for (const std::string &file : node.files)
          rows.emplace_back(file, std::format("node {}", node.name));
    for (const VP::Node &node : view.nodes)
      if (node.name.find('.') == std::string::npos && !VP::is_top(view, node.name))
        list(view, node, 0, rows);
    reply_rows(call, rows);
  }

  // What each folder is reads down one column.
  static void reply_rows(VP::Call &call, std::span<const Rows::value_type> rows) {
    std::size_t width = 0;
    for (const auto &[folder, note] : rows)
      if (!note.empty())
        width = std::max(width, folder.size());
    for (const auto &[folder, note] : rows)
      call.reply(note.empty() ? folder
                              : std::string(folder)
                                    .append(width - folder.size(), ' ')
                                    .append(note_gap)
                                    .append(note));
  }

  // A node's folder, its files, then the folders of the nodes inside it, each in the
  // manifest's order.
  static void
  list(const VP::View &view, const VP::Node &node, std::size_t depth, Rows &rows) {
    const std::string indent(2 * depth, ' ');
    const std::string_view name =
        std::string_view(node.name).substr(node.name.rfind('.') + 1);
    rows.emplace_back(indent + std::string(name) + '/',
                      node.recipe.empty() ? "" : std::format("recipe {}", node.recipe));
    for (const std::string &file : node.files)
      rows.emplace_back(indent + "  " + file, "");
    const std::string inside = node.name + '.';
    for (const VP::Node &inner : view.nodes)
      if (inner.name.starts_with(inside) &&
          inner.name.find('.', inside.size()) == std::string::npos)
        list(view, inner, depth + 1, rows);
  }

  // In the manifest's words, so what info shows is what a node section would hold, then
  // the files its folder holds, which no word lists (RV08), and its connections; or a
  // view section, its file whole, with the connections that take its window.
  static void info(VP::Call &call, const VP::View &view, std::string_view name) {
    if (const auto child = std::ranges::find(view.children, name, &VP::Child::name);
        child != view.children.end()) {
      call.reply(std::format("file = {}", child->file.string()));
      for (const VP::Connection &connection : view.connections)
        if (connection.from.view() && connection.from.node == name)
          call.reply(std::format("writes its window as {}", connection.name));
      return;
    }
    const auto node = std::ranges::find(view.nodes, name, &VP::Node::name);
    if (node == view.nodes.end()) {
      if (!inside(call, view, name) &&
          !info_path(call, view, call.resolved(name), false))
        throw std::runtime_error(
            std::format("no node, view, file or folder is named {}", name));
      return;
    }
    // Its folder as the view runs, which a recipe a node uses unfolds into the library's.
    const std::filesystem::path folder = folder_of(call, name);
    for (const VP::Word &word : VP::words_of(*node))
      call.reply(std::format("{} = {}", word.key, word.value));
    if (!folder.empty())
      call.reply(std::format("folder = {}/", shown(folder)));
    for (const std::string &file : node->files)
      call.reply(std::format("holds {}, {} lines", file, lines_in(call, folder / file).all()));
    for (const VP::Connection &connection : view.connections) {
      if (connection.from.node == name)
        call.reply(std::format("writes {} as {}", connection.from.port, connection.name));
      for (const VP::Endpoint &end : connection.to)
        if (end.node == name)
          call.reply(
              std::format("reads {} from {}", end.port, connection.from.text()));
    }
  }

  // A node's folder as the view the line addresses runs it; none for a node it lacks.
  static std::filesystem::path folder_of(VP::Call &call, std::string_view name) {
    const VP::View &running = call.unfolded();
    const auto node = std::ranges::find(running.nodes, name, &VP::Node::name);
    return node == running.nodes.end() ? std::filesystem::path()
                                       : node->folder.lexically_normal();
  }

  // A file's lines, none for one that cannot be read.
  static Lines lines_in(VP::Call &call, const std::filesystem::path &file) {
    try {
      return lines_of(call.files().read(file.string()), comment_of(file));
    } catch (const std::runtime_error &) {
      return {.text = false};
    }
  }

  // A file or a folder by its path: where it is, the node whose folder it is or is in, and
  // a file's lines and bytes, or what a folder holds, a name a line, and the lines of the
  // files among them, which info shows of each; under a header of its own when selected.
  // Whether there is one.
  static bool info_path(VP::Call &call,
                        const VP::View &view,
                        const std::filesystem::path &absolute,
                        bool header) {
    std::filesystem::path path = absolute.lexically_normal();
    if (path.filename().empty())
      path = path.parent_path();
    const std::vector<std::string> names = call.files().list(path.parent_path().string());
    const std::string name = path.filename().string();
    const bool folder = std::ranges::find(names, name + '/') != names.end();
    if (!folder && std::ranges::find(names, name) == names.end())
      return false;
    const std::string where = shown(path) + (folder ? "/" : "");
    if (header)
      call.reply(std::format("[{} \"{}\"]", folder ? "folder" : "file", where));
    call.reply(std::format("path = {}", where));
    const std::filesystem::path holder = folder ? path : path.parent_path();
    for (const VP::Node &node : view.nodes)
      if (folder_of(call, node.name) == holder)
        call.reply(std::format("node = {}", node.name));
    if (!folder) {
      const std::string text = call.files().read(path.string());
      if (const Lines lines = lines_of(text, comment_of(path)); lines.text)
        call.reply(std::format("lines = {}", said(lines)));
      call.reply(std::format("bytes = {}", text.size()));
      return true;
    }
    Lines lines;
    std::size_t files = 0;
    for (const std::string &held : call.files().list(path.string())) {
      call.reply(std::format("holds {}", held));
      if (held.ends_with('/'))
        continue;
      lines.add(lines_in(call, path / held));
      ++files;
    }
    call.reply(std::format("lines = {} in its {} file{}", said(lines), files,
                           files == 1 ? "" : "s"));
    return true;
  }

  // A node inside a recipe a node uses lives in the recipe's view.vlp; the view holds
  // only the params that node sets on it. Whether the name is one.
  static bool inside(VP::Call &call, const VP::View &view, std::string_view name) {
    for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
         dot = name.find('.', dot + 1)) {
      const auto user =
          std::ranges::find(view.nodes, name.substr(0, dot), &VP::Node::name);
      if (user == view.nodes.end() || !user->uses())
        continue;
      call.reply(std::format(
          "inside recipe {}, which node {} uses as it is", user->recipe, user->name));
      const std::string prefix = std::format("{}.", name.substr(dot + 1));
      for (const VP::Param &param : user->params)
        if (param.key.starts_with(prefix))
          call.reply(
              std::format("param = {}={}", param.key.substr(prefix.size()), param.value));
      return true;
    }
    return false;
  }

  // The view as it runs, each node a row under the node it is inside, as ls lists them:
  // its cost on the CPU and on the GPU, each with its bar, then the frame's. A node in
  // a recipe a node uses is the using node's.
  static void stats(VP::Call &call, std::string_view under) {
    const std::span<const VP::Cost> costs = call.costs();
    if (costs.empty())
      throw std::runtime_error("the view runs no node yet");
    const Tree tree = tree_of(costs);
    std::vector<std::size_t> roots = tree.inside.back();
    if (!under.empty()) {
      const auto found = std::ranges::find(costs, under, &VP::Cost::node);
      if (found == costs.end())
        throw std::runtime_error(std::format(
            "no node named {} runs in the view; probe stats lists them", under));
      roots.assign(1, static_cast<std::size_t>(found - costs.begin()));
    }
    Rows rows;
    for (const std::size_t root : roots)
      cost_rows(costs, tree, root, roots, 0, rows);
    std::size_t width = 0;
    for (const auto &[name, columns] : rows)
      width = std::max(width, name.size());
    call.reply(std::format(
        "{:<{}}  {:<{}} {}", "ms a frame", width, "cpu", 8 + bar_room, "gpu"));
    for (const auto &[name, columns] : rows)
      call.reply(std::format("{:<{}}  {}", name, width, columns));
    Weight frame;
    for (const std::size_t root : roots) {
      frame.cpu += tree.sums[root].cpu;
      frame.gpu += tree.sums[root].gpu;
      frame.runs = frame.runs || tree.sums[root].runs;
      frame.untimed = frame.untimed || tree.sums[root].untimed;
    }
    call.reply(std::format(
        "in all: {:.3f} ms on the CPU{}{}",
        frame.cpu,
        frame.runs ? std::format(", {:.3f} ms on the GPU", frame.gpu) : std::string(),
        frame.untimed ? ", a ? marking a pass the GPU did not time" : ""));
  }

  // A node's row, then the rows of the nodes inside it, each bar to the costliest of
  // those beside it; a node that neither cooks nor runs a pass, nor holds one that does,
  // as one holding only files, costs nothing and has none.
  static void cost_rows(std::span<const VP::Cost> costs,
                        const Tree &tree,
                        std::size_t at,
                        std::span<const std::size_t> beside,
                        std::size_t depth,
                        Rows &rows) {
    if (!tree.sums[at].cooks && !tree.sums[at].runs)
      return;
    float most_cpu = 0.0f;
    float most_gpu = 0.0f;
    for (const std::size_t other : beside) {
      most_cpu = std::max(most_cpu, tree.sums[other].cpu);
      most_gpu = std::max(most_gpu, tree.sums[other].gpu);
    }
    const std::string_view name = costs[at].node;
    const Weight &sum = tree.sums[at];
    rows.emplace_back(
        std::string(2 * depth, ' ')
            .append(depth == 0 ? name : name.substr(name.rfind('.') + 1)),
        column(sum.cpu, most_cpu, sum.cooks, false) + ' ' +
            column(sum.gpu, most_gpu, sum.runs, sum.untimed));
    for (const std::size_t inner : tree.inside[at])
      cost_rows(costs, tree, inner, tree.inside[at], depth + 1, rows);
  }

  // <view>:<node> of a graph in a window, whose inside param it reads, and of the node it
  // shows a folder from, whose folder param.
  std::string _graph;
  std::string _folders;
  VP::Command _ls;
  VP::Command _info;
  VP::Command _stats;
};

} // namespace

VP_OPERATOR(Inspect);
