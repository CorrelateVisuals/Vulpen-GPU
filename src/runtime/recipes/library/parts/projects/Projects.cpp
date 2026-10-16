#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view manifest = "view.vlp";

// Letters, digits, _ and -, so a project's name also addresses its view, as `name: ls`,
// and runs it, as ./run.sh --name.
bool project_name(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
           (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
  });
}

void check_name(std::string_view name) {
  if (!project_name(name))
    throw std::runtime_error(std::format(
        "{} is no project's name, which is letters, digits, _ and -; a path to a folder "
        "holds a /",
        name));
}

// A view is named for its folder, which the build tree mirrors, and no two hosted views
// share a name, so one hosted already is named, as its tab names it.
void check_free(VP::Call &call, std::string_view name) {
  if (const VP::View *const hosted = call.view(name))
    throw std::runtime_error(std::format(
        "a view named {} is hosted already, from {}", name, hosted->file.string()));
}

// The name the view a command addresses is hosted by beside this one. The view vulpen
// started with cannot go, and a view a project hosts goes with that project.
std::string hosted_name(VP::Call &call) {
  const VP::View &view = call.view();
  const VP::View &top = *call.view("");
  if (view.file == top.file)
    throw std::runtime_error(std::format(
        "{} is the view vulpen started with, which runs until the end; this works on a "
        "view it hosts",
        view.file.string()));
  for (const VP::Child &child : top.children)
    if (child.file.lexically_normal() == view.file.lexically_normal())
      return child.name;
  throw std::runtime_error(std::format(
      "{} is hosted by a project, whose folder holds it, so it goes with that project",
      view.file.string()));
}

// The projects: views of their own in one folder (V03), which the commands here make,
// host, copy and rename, by a project's name or by a folder's path.
class Projects final : public VP::Operator {
  void bind(VP::Bind &node) override {
    // From the part's own folder (RP02), so a person who works elsewhere changes one
    // param.
    _folder = (std::filesystem::path(node.folder()) / node.param<std::string>("folder"))
                  .lexically_normal();
    _new = node.command("view new <view>",
                        "hosts a new, empty view, a project's by name or a folder's by "
                        "path, and saves its view.vlp");
    _load = node.command("view load <view>",
                         "hosts the view of a project by name, or of a folder by path");
    _save_as = node.command("view save as <name>",
                            "copies the view, its edits too, into a new project and "
                            "works on the copy; the view's own files stay as last saved");
    _rename = node.command("view rename <name>",
                           "saves the view, renames its folder where it is, and hosts it "
                           "under the new name");
  }

  void command(VP::Call &call) override {
    if (call.is(_new) || call.is(_load))
      host(call, folder_of(call.arguments().front()), call.is(_new));
    else if (call.is(_save_as))
      save_as(call);
    else if (call.is(_rename))
      rename(call);
  }

  // A <view> comes resolved when it is a path, and as typed when it names a project.
  std::filesystem::path folder_of(std::string_view word) const {
    const std::filesystem::path path(word);
    if (path.is_relative()) {
      check_name(word);
      return _folder / path;
    }
    return path.filename().empty() ? path.parent_path() : path;
  }

  static void host(VP::Call &call, const std::filesystem::path &folder, bool fresh) {
    const std::string name = folder.filename().string();
    check_free(call, name);
    const std::vector<std::string> names = call.files().list(folder.string());
    const bool exists = std::ranges::find(names, manifest) != names.end();
    if (fresh && exists)
      throw std::runtime_error(
          std::format("{} holds a view already; view load hosts it", folder.string()));
    if (!fresh && !exists)
      throw std::runtime_error(std::format(
          "{} holds no {}; view new starts a view there", folder.string(), manifest));
    call.commands().send(
        std::format(": view add {} {}", name, (folder / manifest).string()));
    if (fresh)
      call.commands().send(std::format("{}: view save", name));
  }

  // The copy takes the view's folder whole, its look too, then the manifest as the edits
  // left it, so the view's own files stay as last saved. A view its folder holds would be
  // copied under the same name, and the build holds one view of each name.
  void save_as(VP::Call &call) const {
    const std::string hosted = hosted_name(call);
    const std::string name(call.arguments().front());
    check_name(name);
    check_free(call, name);
    const VP::View &view = call.view();
    const std::filesystem::path folder = view.file.parent_path();
    for (const VP::Child &child : view.children)
      if (const std::filesystem::path inside = child.file.lexically_relative(folder);
          !inside.empty() && *inside.begin() != "..")
        throw std::runtime_error(std::format(
            "{} hosts {} from its folder, whose copy would be a second view named {}, "
            "and the build holds one view of each name",
            view.name,
            child.name,
            child.name));
    const std::filesystem::path to = _folder / name;
    call.files().copy(folder.string(), to.string());
    call.files().save(view, (to / manifest).string());
    call.commands().send(std::format(": view close {}", hosted));
    call.commands().send(std::format(": view add {} {}", name, (to / manifest).string()));
  }

  // The folder moves whole, with the views it hosts. Nothing inside a project names it,
  // as the build names its contracts and modules from the folder, so nothing else
  // changes.
  static void rename(VP::Call &call) {
    const std::string hosted = hosted_name(call);
    const std::string name(call.arguments().front());
    check_name(name);
    check_free(call, name);
    // Kept, since the view addressed goes once it is closed.
    const std::filesystem::path file = call.view().file;
    const std::filesystem::path to = file.parent_path().parent_path() / name;
    call.commands().send(std::format("{}: view save", hosted));
    call.files().rename(file.parent_path().string(), to.string());
    call.commands().send(std::format(": view close {}", hosted));
    call.commands().send(
        std::format(": view add {} {}", name, (to / file.filename()).string()));
  }

  std::filesystem::path _folder;
  VP::Command _new;
  VP::Command _load;
  VP::Command _save_as;
  VP::Command _rename;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Projects>("Projects");
}
