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

constexpr std::string_view manifest = VP::manifest_file;

// A node's name, so a project's name also addresses its view, as `name: ls`, and runs
// it, as ./run.sh --name.
void check_name(std::string_view name) {
  if (!VP::is_name(name))
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

// The projects: views of their own in one folder (V03), or in a folder there that groups
// some and is no view itself, as triangles/ groups the triangle's variants, which the
// commands here make, host, copy and rename, by a project's name or by a folder's path;
// each view they host becomes the current one.
class Projects final : public VP::Operator {
  void bind(VP::Bind &node) override {
    // From the part's own folder (RP02), so a person who works elsewhere changes one
    // param.
    _folder = (std::filesystem::path(node.folder()) / node.param<std::string>("folder"))
                  .lexically_normal();
    _apps = (std::filesystem::path(node.folder()) / node.param<std::string>("apps"))
                .lexically_normal();
    _new = node.command("view new <project>",
                        "hosts a new, empty view, a project's by name or a folder's by "
                        "path, and saves its view.vlp");
    _load = node.command("view load <project>",
                         "hosts the view of a project by name, or of a folder by path; "
                         "an app's name, as ide, hosts the app beside the projects again");
    _save_as = node.command("view save as <name>",
                            "copies the view, its edits too, into a new project and "
                            "works on the copy; the view's own files stay as last saved");
    _rename = node.command("view rename <name>",
                           "saves the view, renames its folder where it is, and hosts it "
                           "under the new name");
    _remove = node.command("view remove <project>",
                           "closes a project, if it is hosted, and deletes its folder "
                           "with all it holds, as node remove does a node's");
  }

  void command(VP::Call &call) override {
    const std::string_view word = call.arguments().empty() ? "" : call.arguments().front();
    if (call.is(_load) && VP::is_name(word) && found(call, word).empty() &&
        holds_view(call, _apps / word))
      host_app(call, _apps / word);
    else if (call.is(_new) || call.is(_load))
      host(call, folder_of(call, word), call.is(_new));
    else if (call.is(_save_as))
      save_as(call);
    else if (call.is(_rename))
      rename(call);
    else if (call.is(_remove))
      remove(call);
  }

  // A <project> comes resolved when it is a path, and as typed when it names one, which
  // a new project takes in the projects' folder; a path to a project's view.vlp names its
  // folder, as ./run.sh takes either.
  std::filesystem::path folder_of(VP::Call &call, std::string_view word) const {
    const std::filesystem::path path(word);
    if (path.is_relative()) {
      check_name(word);
      const std::filesystem::path there = found(call, word);
      return there.empty() ? _folder / path : there;
    }
    return path.filename().empty() || path.filename() == manifest ? path.parent_path()
                                                                   : path;
  }

  // The folder of the project of that name, among the projects or in a folder grouping
  // some; none where no folder of that name holds a view.
  std::filesystem::path found(VP::Call &call, std::string_view name) const {
    if (holds_view(call, _folder / name))
      return _folder / name;
    for (const std::string &entry : call.files().list(_folder.string()))
      if (entry.ends_with('/') && !holds_view(call, _folder / entry) &&
          holds_view(call, _folder / entry / name))
        return _folder / entry / name;
    return {};
  }

  static bool holds_view(VP::Call &call, const std::filesystem::path &folder) {
    const std::vector<std::string> names = call.files().list(folder.string());
    return std::ranges::find(names, manifest) != names.end();
  }

  // An app, as the ide, hosted beside the projects as vulpen hosts it at the start, after
  // a view close took it. The current view stays: an app's view is a window to work in,
  // not one to work on.
  static void host_app(VP::Call &call, const std::filesystem::path &folder) {
    const std::string name = folder.filename().string();
    if (call.view("")->file.lexically_normal() == (folder / manifest).lexically_normal())
      throw std::runtime_error(
          std::format("{} is the view vulpen started with, which runs already", name));
    check_free(call, name);
    call.commands().send(
        std::format(": view add {} {}", name, (folder / manifest).string()));
  }

  // An app's folder is the library's to keep, as the build and ./run.sh name it, and its
  // nodes use recipes as they are, which no project's may; so only a project is renamed
  // or copied into one.
  void check_project(VP::Call &call, std::string_view hosted) const {
    const std::filesystem::path inside = call.view().file.lexically_relative(_apps);
    if (!inside.empty() && *inside.begin() != "..")
      throw std::runtime_error(std::format(
          "{} is an app beside the library, not a project, so its folder stays as it is; "
          "recipe drop copies a recipe into a project",
          hosted));
  }

  // The build holds one view of each name, so a new project's name is one no project has,
  // in a group or not.
  void check_unused(VP::Call &call, std::string_view name) const {
    if (const std::filesystem::path there = found(call, name); !there.empty())
      throw std::runtime_error(std::format(
          "{} holds a project named {} already, and the build holds one view of each name",
          there.string(),
          name));
  }

  void host(VP::Call &call, const std::filesystem::path &folder, bool fresh) const {
    const std::string name = folder.filename().string();
    check_free(call, name);
    const bool exists = holds_view(call, folder);
    if (fresh && exists)
      throw std::runtime_error(
          std::format("{} holds a view already; view load hosts it", folder.string()));
    if (fresh && VP::is_name(name))
      check_unused(call, name);
    if (!fresh && !exists)
      throw std::runtime_error(std::format(
          "{} holds no {}; view new starts a view there", folder.string(), manifest));
    call.commands().send(
        std::format(": view add {} {}", name, (folder / manifest).string()));
    if (fresh)
      call.commands().send(std::format("{}: view save", name));
    call.commands().send(std::format("view switch {}", name));
  }

  // The copy takes the view's folder whole, its look too, then the manifest as the edits
  // left it, so the view's own files stay as last saved. A view its folder holds would be
  // copied under the same name, and the build holds one view of each name.
  void save_as(VP::Call &call) const {
    const std::string hosted = hosted_name(call);
    check_project(call, hosted);
    const std::string name(call.arguments().front());
    check_name(name);
    check_free(call, name);
    check_unused(call, name);
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
    call.commands().send(std::format("view switch {}", name));
  }

  // The folder moves whole, with the views it hosts. Nothing inside a project names it,
  // as the build names its contracts and modules from the folder, so nothing else
  // changes.
  void rename(VP::Call &call) const {
    const std::string hosted = hosted_name(call);
    check_project(call, hosted);
    const std::string name(call.arguments().front());
    check_name(name);
    check_free(call, name);
    check_unused(call, name);
    // Kept, since the view addressed goes once it is closed.
    const std::filesystem::path file = call.view().file;
    const std::filesystem::path to = file.parent_path().parent_path() / name;
    call.commands().send(std::format("{}: view save", hosted));
    call.files().rename(file.parent_path().string(), to.string());
    call.commands().send(std::format(": view close {}", hosted));
    call.commands().send(
        std::format(": view add {} {}", name, (to / file.filename()).string()));
    call.commands().send(std::format("view switch {}", name));
  }

  // By a project's name alone, so no path reaches an app's folder or one elsewhere. Closed
  // first, as a folder holding a view that runs is never deleted, then deleted by the
  // files part's line, which refuses a folder holding another view that runs, naming it.
  void remove(VP::Call &call) const {
    const std::string_view name = call.arguments().front();
    if (!VP::is_name(name))
      throw std::runtime_error(std::format(
          "{} is no project's name, which is letters, digits, _ and -, as view list "
          "shows it; view remove takes no path",
          name));
    const std::filesystem::path folder = found(call, name);
    if (folder.empty())
      throw std::runtime_error(std::format("no project is named {}: {} holds no {}",
                                           name,
                                           (_folder / name).string(),
                                           manifest));
    if (const VP::View *const hosted = call.view(name);
        hosted && hosted->file.lexically_normal() == (folder / manifest).lexically_normal())
      call.commands().send(std::format(": view close {}", name));
    call.reply(call.commands().send(std::format("file remove force {}/", folder.string())));
  }

  std::filesystem::path _folder;
  std::filesystem::path _apps; // the recipes' folder, where each app beside the library is
  VP::Command _new;
  VP::Command _load;
  VP::Command _save_as;
  VP::Command _rename;
  VP::Command _remove;
};

} // namespace

VP_OPERATOR(Projects);
