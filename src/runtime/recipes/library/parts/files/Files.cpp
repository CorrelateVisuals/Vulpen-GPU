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

// Where a path lies in a view vulpen hosts: the view, as the edits so far left it, and
// the path from its folder, empty for the folder itself.
struct Held {
  const VP::View *view = nullptr;
  std::filesystem::path inside;
};

// From the view vulpen started with down the views each hosts, the one whose folder holds
// the path nearest; none for a path no hosted view's folder holds, which is a folder like
// any other, as a project not loaded is.
std::optional<Held> held_by(VP::Call &call, const std::filesystem::path &path) {
  std::optional<Held> nearest;
  std::vector<std::string> names{std::string()};
  for (std::size_t at = 0; at < names.size(); ++at) {
    const VP::View *const view = call.view(names[at]);
    if (!view)
      continue;
    for (const VP::Child &child : view->children)
      names.push_back(child.name);
    std::filesystem::path inside = path.lexically_relative(
        std::filesystem::absolute(view->file.parent_path()).lexically_normal());
    if (inside.empty() || *inside.begin() == "..")
      continue;
    if (inside == ".")
      inside.clear();
    if (!nearest || std::distance(inside.begin(), inside.end()) <
                        std::distance(nearest->inside.begin(), nearest->inside.end()))
      nearest = Held{view, inside};
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

// Every folder in a view is a node's (RV08), so a line making or filling a folder there
// that no node names would part the two: each folder on the way to the path, and the path
// itself when it is a folder, must be a node's, or lie where no node is.
void check_folders(const Held &held, bool folder) {
  std::filesystem::path inside;
  for (const std::filesystem::path &part : held.inside) {
    inside /= part;
    if (inside == held.inside && !folder)
      break;
    const std::optional<std::string> node = node_of(inside);
    if (node && !named(*held.view, *node))
      throw std::runtime_error(std::format(
          "{}/ in view {} would be a folder no node names, and every folder in a view is "
          "a node's (RV08): node add {} names it first",
          inside.generic_string(),
          held.view->name,
          *node));
  }
}

// A view's folder and manifest, and a node's folder, move and go only with the lines
// that keep a view and its folders together, which the refusal names (A02).
void check_kept(const std::optional<Held> &held,
                const std::filesystem::path &path,
                bool renaming) {
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
      node && named(*held->view, *node))
    throw std::runtime_error(
        renaming ? std::format("{}/ is node {}'s folder, which node rename {} <name> moves "
                               "with the node",
                               shown(path),
                               *node,
                               *node)
                 : std::format("{}/ is node {}'s folder, which stays until node remove {} "
                               "takes the node",
                               shown(path),
                               *node,
                               *node));
}

// Files and folders made, renamed, copied and deleted through the file port (V05), so a
// person does all of it from inside vulpen, by lines a GUI sends too (V06). A path counts
// from where vulpen started. In a view vulpen hosts every folder is a node's (RV08): a
// line that would make a folder there no node names, or move or delete a node's folder or
// the view's own, is refused, naming the line that keeps them together (A02). Nothing is
// written over: a line whose path is taken is refused, and a folder holding files goes
// only with force, as quit force ends a run that would lose work.
class Files final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _new = node.command("file new <file>",
                        "makes an empty file, and the folders it is in; a path ending in "
                        "/ makes a folder; in a view a folder is a node's, which node add "
                        "names first");
    _rename = node.command("file rename <file> <file>",
                           "renames a file or a folder, or moves it, into folders made "
                           "for it; a node's folder moves with node rename, a view's "
                           "with view rename");
    _copy = node.command("file copy <file> <file>",
                         "copies a file, or a folder with all it holds, whole or not at "
                         "all");
    _remove = node.command("file remove <file>",
                           "deletes a file or an empty folder; a node's folder once node "
                           "remove took the node, a view's once view close let it go");
    _remove_force = node.command("file remove force <file>",
                                 "deletes a folder with all it holds, as file remove does "
                                 "the rest");
  }

  void command(VP::Call &call) override {
    const std::span<const std::string_view> words = call.arguments();
    if (call.is(_new))
      make(call, words[0]);
    else if (call.is(_rename) || call.is(_copy))
      move(call, bare(words[0]), bare(words[1]), call.is(_copy));
    else if (call.is(_remove) || call.is(_remove_force))
      remove(call, bare(words[0]), call.is(_remove_force));
  }

  static void make(VP::Call &call, std::string_view word) {
    const std::filesystem::path path = bare(word);
    const bool folder = std::filesystem::path(word).filename().empty();
    if (there(call.files(), path))
      throw std::runtime_error(std::format("{} is there already", shown(path)));
    if (const std::optional<Held> held = held_by(call, path))
      check_folders(*held, folder);
    if (folder)
      call.files().make(path.string());
    else
      call.files().save(path.string(), {});
    call.reply(std::format("made {}{}", shown(path), folder ? "/" : ""));
  }

  // A rename takes what it names along, so a node's folder or a view's own stays where
  // its name puts it; a copy leaves it, so only where the copy lands counts.
  static void move(VP::Call &call,
                   const std::filesystem::path &from,
                   const std::filesystem::path &to,
                   bool copy) {
    if (!copy)
      check_kept(held_by(call, from), from, true);
    if (const std::optional<Held> held = held_by(call, to))
      check_folders(*held, listed(call.files(), from, "/"));
    if (copy)
      call.files().copy(from.string(), to.string());
    else
      call.files().rename(from.string(), to.string());
    call.reply(
        std::format("{} {} to {}", copy ? "copied" : "renamed", shown(from), shown(to)));
  }

  static void remove(VP::Call &call, const std::filesystem::path &path, bool force) {
    VP::FilePort &files = call.files();
    if (!there(files, path))
      throw std::runtime_error(std::format("{} is not there", shown(path)));
    check_kept(held_by(call, path), path, false);
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

  VP::Command _new;
  VP::Command _rename;
  VP::Command _copy;
  VP::Command _remove;
  VP::Command _remove_force;
};

} // namespace

VP_OPERATOR(Files);
