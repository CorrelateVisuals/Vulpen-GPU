#include "runtime/Operator.h"
#include "runtime/View.h"

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The person's editor: open hands a file or a folder to the program its program param
// names, or, while the view its window param names runs, a file to the editor there,
// through that editor's file param, as a seam's drag sets a split's ratio. So open is one
// command with one owner, whichever editor takes the file (V06).
class Editor final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _program = node.param<std::string>("program");
    // <view>:<node>, with no view for the one vulpen started with; unset, every file goes
    // to the program.
    _window = node.sets("window") ? node.param<std::string>("window") : std::string();
    _name = node.name();
    _open = node.command("open [<file>]",
                         "opens a file in the editor in a window while one runs, else a "
                         "file or a folder in the person's editor; with none, the view's "
                         "manifest there, or its folder here");
  }

  void command(VP::Call &call) override {
    const std::size_t colon = _window.find(':');
    if (colon != std::string::npos && call.view(_window.substr(0, colon)))
      in_window(call, _window.substr(0, colon), _window.substr(colon + 1));
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
  std::string _name;
  VP::Command _open;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Editor>("Editor");
}
