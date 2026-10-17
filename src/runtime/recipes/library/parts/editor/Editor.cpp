#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Whether a file or a folder is there, as its folder lists it.
bool named(VP::FilePort &files, std::filesystem::path path) {
  if (path.filename().empty())
    path = path.parent_path();
  const std::vector<std::string> names = files.list(path.parent_path().string());
  const std::string name = path.filename().string();
  return std::ranges::find(names, name) != names.end() ||
         std::ranges::find(names, name + '/') != names.end();
}

// Whether a path names a folder, as its folder lists it with a / after its name.
bool folder(VP::FilePort &files, std::filesystem::path path) {
  if (path.filename().empty())
    path = path.parent_path();
  const std::vector<std::string> names = files.list(path.parent_path().string());
  return std::ranges::find(names, path.filename().string() + '/') != names.end();
}

// A target of open's, <view>:<node>, while that view runs: the view and the node.
std::optional<std::pair<std::string, std::string>> running(VP::Call &call,
                                                           std::string_view target) {
  const std::size_t colon = target.find(':');
  if (colon == std::string_view::npos || !call.view(target.substr(0, colon)))
    return std::nullopt;
  return std::pair{std::string(target.substr(0, colon)),
                   std::string(target.substr(colon + 1))};
}

// The files of that name a view holds, in its nodes' folders (RV08), as it runs, each
// recipe a node uses unfolded, so the ide's nodes hold their recipes' files; as the view
// holds them, while its manifest cannot be read, as a new project's before its save. A
// recipe two nodes use shows once.
std::vector<std::filesystem::path> held(VP::Call &call, const std::string &name) {
  VP::View view = call.view();
  try {
    view = call.files().manifest(view.file.string());
  } catch (const std::runtime_error &) {
    for (VP::Node &node : view.nodes) {
      std::string folder = node.name;
      std::ranges::replace(folder, '.', '/');
      node.folder = view.file.parent_path() / folder;
    }
  }
  std::vector<std::filesystem::path> found;
  for (const VP::Node &node : view.nodes)
    if (std::ranges::find(node.files, name) != node.files.end() &&
        std::ranges::find(found, node.folder / name) == found.end())
      found.push_back(node.folder / name);
  return found;
}

// A path as the prompt names a folder, from where vulpen started.
std::string shown(const std::filesystem::path &path) {
  return path.lexically_proximate(std::filesystem::current_path()).generic_string();
}

// The person's editor: open hands a file or a folder to the program its program param
// names, or, while the view its window param names runs, a file to the editor there,
// through that editor's file param, as a seam's drag sets a split's ratio, and while the
// view its folders param names runs, a folder to the node there, through its folder
// param, as the relations part the graph shows a folder from. So open is one command
// with one owner, whichever takes the file or the folder (V06).
class Editor final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _program = node.param<std::string>("program");
    // <view>:<node>, with no view for the one vulpen started with; unset, every file goes
    // to the program, and every folder.
    _window = node.sets("window") ? node.param<std::string>("window") : std::string();
    _folders = node.sets("folders") ? node.param<std::string>("folders") : std::string();
    _name = node.name();
    _open = node.command("open [<path>]",
                         "opens a file in the editor in a window while one runs, and a "
                         "folder in the graph there, else either in the person's editor; "
                         "with none, the view's manifest there, or its folder here");
  }

  // A word without a / names a file the view holds, as a project's name does for view
  // load, which two of that name make a mistake; a path names any. A path that names
  // nothing is refused, whichever editor takes it, so a misspelt one fails loud rather
  // than opening as a new, empty file (A02).
  void command(VP::Call &call) override {
    const std::string path = path_of(call);
    const auto folders = running(call, _folders);
    const auto window = running(call, _window);
    if (folders && !path.empty() && folder(call.files(), path))
      call.commands().send(std::format(
          "{}: param set {} folder {}", folders->first, folders->second, path));
    else if (window)
      in_window(call, window->first, window->second, path);
    else
      in_program(call, path);
  }

  // The file or folder open names, or none.
  std::string path_of(VP::Call &call) const {
    if (call.arguments().empty())
      return {};
    const std::string word(call.arguments()[0]);
    if (word.find('/') == std::string::npos) {
      const std::vector<std::filesystem::path> found = held(call, word);
      if (found.size() == 1)
        return found.front().string();
      std::string files;
      for (const std::filesystem::path &file : found)
        files.append(files.empty() ? "" : ", ").append(shown(file));
      throw std::runtime_error(
          found.empty()
              ? std::format("the view holds no file named {}; a path names any, as ./{} "
                            "from where vulpen started",
                            word,
                            word)
              : std::format("the view holds {} files named {}: {}; a path names one",
                            found.size(),
                            word,
                            files));
    }
    if (!named(call.files(), word)) {
      const std::filesystem::path path(word);
      const std::vector<std::filesystem::path> there =
          held(call, path.filename().string());
      throw std::runtime_error(std::format(
          "{} names nothing, as a typed path counts from where vulpen started{}",
          path.string(),
          there.empty() ? "" : std::format("; the view holds {}", shown(there.front()))));
    }
    return word;
  }

  // The editor in the window opens files, so a folder, or a file it cannot read, is
  // refused here, naming the program's way, before the editor's param names it.
  void in_window(VP::Call &call,
                 std::string_view view,
                 std::string_view editor,
                 const std::string &path) const {
    const std::string file = path.empty() ? call.view().file.string() : path;
    try {
      call.files().read(file);
    } catch (const std::runtime_error &failure) {
      throw std::runtime_error(
          std::format("{}; the editor in the window opens files, and `: param unset {} "
                      "window` sends each to {}",
                      failure.what(),
                      _name,
                      _program));
    }
    call.commands().send(std::format("{}: param set {} file {}", view, editor, file));
  }

  void in_program(VP::Call &call, const std::string &path) const {
    const std::vector<std::string> arguments{
        path.empty() ? call.view().file.parent_path().string() : path};
    try {
      call.processes().start(_program, arguments);
    } catch (const std::runtime_error &failure) {
      throw std::runtime_error(std::format(
          "{}; `: param set {} program <name>` names another", failure.what(), _name));
    }
  }

  std::string _program;
  std::string _window;
  std::string _folders;
  std::string _name;
  VP::Command _open;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Editor>("Editor");
}
