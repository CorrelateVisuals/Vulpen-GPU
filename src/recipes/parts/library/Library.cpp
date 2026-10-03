#include "runtime/Operator.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

// The folders in a folder, each named with the / that marks it.
std::vector<std::string> folders(VP::FilePort &files, const std::filesystem::path &in) {
  std::vector<std::string> found;
  for (std::string &name : files.list(in.string()))
    if (name.ends_with('/'))
      found.push_back(std::move(name));
  return found;
}

// Works on recipe and view folders through the file port: lists and drops recipes,
// starts one from the template, and makes and loads views. A drop copies files and then
// deploys, so the view owns its copy from the first edit on.
class Library final : public VP::Operator {
  void bind(VP::Bind &node) override {
    // A part's folder is <library>/<kind>/<name>.
    _library = std::filesystem::path(node.folder()).parent_path().parent_path();
    _list = node.command("recipe list", "lists the library's recipes, by kind");
  }

  void command(VP::Call &call) override {
    if (call.is(_list))
      list(call);
  }

  // A recipe is a folder in a kind's folder; the contracts' folder holds only files.
  void list(VP::Call &call) const {
    for (const std::string &kind : folders(call.files(), _library))
      for (std::string recipe : folders(call.files(), _library / kind)) {
        recipe.pop_back();
        call.reply(kind + recipe);
      }
  }

  std::filesystem::path _library;
  VP::Command _list;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<Library>("Library");
}
