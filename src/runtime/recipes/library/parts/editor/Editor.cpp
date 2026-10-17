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

// Where the view holds a file of that name, in a node's folder (RV08), as the prompt
// names a folder, from where vulpen started; empty for nowhere.
std::string held(const VP::View &view, const std::string &name) {
  for (const VP::Node &node : view.nodes)
    if (std::ranges::find(node.files, name) != node.files.end()) {
      std::string folder = node.name;
      std::ranges::replace(folder, '.', '/');
      return (view.file.parent_path() / folder / name)
          .lexically_proximate(std::filesystem::current_path())
          .generic_string();
    }
  return {};
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
    _open = node.command("open [<file>]",
                         "opens a file in the editor in a window while one runs, and a "
                         "folder in the graph there, else either in the person's editor; "
                         "with none, the view's manifest there, or its folder here");
  }

  // A path that names nothing is refused, whichever editor takes it, so a misspelt one
  // fails loud rather than opening as a new, empty file (A02).
  void command(VP::Call &call) override {
    if (!call.arguments().empty() && !named(call.files(), call.arguments()[0])) {
      const std::filesystem::path path(call.arguments()[0]);
      const std::string there = held(call.view(), path.filename().string());
      throw std::runtime_error(std::format(
          "{} names nothing, as a typed path counts from where vulpen started{}",
          path.string(),
          there.empty() ? "" : std::format("; the view holds {}", there)));
    }
    const auto folders = running(call, _folders);
    const auto window = running(call, _window);
    if (folders && !call.arguments().empty() && folder(call.files(), call.arguments()[0]))
      call.commands().send(std::format("{}: param set {} folder {}",
                                       folders->first,
                                       folders->second,
                                       call.arguments()[0]));
    else if (window)
      in_window(call, window->first, window->second);
    else
      in_program(call);
  }

  // The editor in the window opens files, so a folder, or a file it cannot read, is
  // refused here, naming the program's way, before the editor's param names it.
  void in_window(VP::Call &call, std::string_view view, std::string_view editor) const {
    const std::string file = call.arguments().empty() ? call.view().file.string()
                                                      : std::string(call.arguments()[0]);
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

  void in_program(VP::Call &call) const {
    const std::vector<std::string> arguments{
        call.arguments().empty() ? call.view().file.parent_path().string()
                                 : std::string(call.arguments()[0])};
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
