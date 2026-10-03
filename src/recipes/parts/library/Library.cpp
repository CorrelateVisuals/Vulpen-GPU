#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view manifest = "view.vlp";
constexpr std::string_view contracts = "contracts";
// How a recipe's shader names a contract (RV05), so a drop knows which to copy.
constexpr std::string_view contract_include = "#include \"contracts/";
constexpr std::string_view blanks = " \t\r";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// The folders in a folder, each named with the / that marks it.
std::vector<std::string> folders(VP::FilePort &files, const std::filesystem::path &in) {
  std::vector<std::string> found;
  for (std::string &name : files.list(in.string()))
    if (name.ends_with('/'))
      found.push_back(std::move(name));
  return found;
}

bool holds(VP::FilePort &files,
           const std::filesystem::path &folder,
           std::string_view name) {
  const std::vector<std::string> names = files.list(folder.string());
  return std::ranges::find(names, name) != names.end();
}

// As text, since neither path need exist.
bool inside(const std::filesystem::path &path, const std::filesystem::path &folder) {
  const std::filesystem::path relative =
      path.lexically_normal().lexically_relative(folder.lexically_normal());
  return !relative.empty() && *relative.begin() != "..";
}

// The recipe words of a manifest: a node's own recipe, and each recipe a deploy brings.
std::vector<std::string> recipes_named(std::string_view text) {
  std::vector<std::string> found;
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::string_view line = text.substr(at, end - at);
    const std::string_view words = line.substr(0, line.find('#'));
    if (const std::size_t equals = words.find('=');
        equals != std::string_view::npos && trim(words.substr(0, equals)) == "recipe")
      found.emplace_back(trim(words.substr(equals + 1)));
    at = end + 1;
  }
  return found;
}

// A folder and everything in it, each file saved whole (RA04), noting the contracts its
// files include.
void copy(VP::FilePort &files,
          const std::filesystem::path &from,
          const std::filesystem::path &to,
          std::set<std::string> &included) {
  for (const std::string &name : files.list(from.string())) {
    if (name.ends_with('/')) {
      const std::string_view folder = std::string_view(name).substr(0, name.size() - 1);
      copy(files, from / folder, to / folder, included);
      continue;
    }
    const std::string text = files.read((from / name).string());
    for (std::size_t at = text.find(contract_include); at != std::string::npos;
         at = text.find(contract_include, at + 1)) {
      const std::size_t start = at + contract_include.size();
      included.insert(text.substr(start, text.find('"', start) - start));
    }
    files.save((to / name).string(), text);
  }
}

// Works on recipe and view folders through the file port: lists and drops recipes, and
// makes and loads views. A drop copies files and then deploys, so the view owns its copy
// from the first edit on (V03).
class Library final : public VP::Operator {
  void bind(VP::Bind &node) override {
    // A part's folder is <library>/<kind>/<name>.
    _library = std::filesystem::path(node.folder()).parent_path().parent_path();
    _list = node.command("recipe list", "lists the library's recipes, by kind");
    _drop = node.command("recipe drop <recipe> <name>",
                         "copies a library recipe into the view, with the recipes it "
                         "deploys and the contracts they include, and deploys it");
    _new = node.command("view new <file>",
                        "hosts a new, empty view in a folder, and saves its view.vlp");
    _load = node.command("view load <file>", "hosts the view in a folder's view.vlp");
  }

  void command(VP::Call &call) override {
    if (call.is(_list))
      list(call);
    else if (call.is(_drop))
      drop(call);
    else if (call.is(_new) || call.is(_load))
      host(call, call.is(_new));
  }

  // A recipe is a folder in a kind's folder; the contracts' folder holds only files.
  void list(VP::Call &call) const {
    for (const std::string &kind : folders(call.files(), _library))
      for (std::string recipe : folders(call.files(), _library / kind)) {
        recipe.pop_back();
        call.reply(kind + recipe);
      }
  }

  // Where a recipe of the library is, by its name.
  std::filesystem::path find(VP::FilePort &files, std::string_view recipe) const {
    const std::string folder = std::string(recipe) + '/';
    for (const std::string &kind : folders(files, _library))
      if (holds(files, _library / kind, folder))
        return _library / kind / recipe;
    throw std::runtime_error(
        std::format("no recipe {} in the library; recipe list lists them", recipe));
  }

  // The recipe, every recipe it deploys and every contract they include, into the view's
  // recipes. A copy the view has already stays: the view owns it (V03).
  void drop(VP::Call &call) const {
    const std::span<const std::string_view> arguments = call.arguments();
    VP::FilePort &files = call.files();
    const std::filesystem::path into = call.view().file.parent_path() / "recipes";
    if (inside(into, _library))
      throw std::runtime_error(std::format(
          "{} is in the library, which deploys its own recipes: deploy add {} {}",
          call.view().file.string(),
          arguments[1],
          arguments[0]));
    std::vector<std::string> recipes{std::string(arguments[0])};
    std::set<std::string> included;
    std::string copied;
    for (std::size_t index = 0; index < recipes.size(); ++index) {
      const std::filesystem::path from = find(files, recipes[index]);
      for (std::string &named : recipes_named(files.read((from / manifest).string())))
        if (std::ranges::find(recipes, named) == recipes.end())
          recipes.push_back(std::move(named));
      if (holds(files, into, recipes[index] + '/'))
        continue;
      copy(files, from, into / recipes[index], included);
      copied.append(copied.empty() ? "" : ", ").append(recipes[index]);
    }
    for (const std::string &contract : included)
      if (!holds(files, into / contracts, contract)) {
        files.save((into / contracts / contract).string(),
                   files.read((_library / contracts / contract).string()));
        copied.append(copied.empty() ? "" : ", ")
            .append(contracts)
            .append("/" + contract);
      }
    call.commands().send(std::format("deploy add {} {}", arguments[1], arguments[0]));
    if (!copied.empty())
      call.reply(std::format("copied {} into {}", copied, into.string()));
  }

  // A view is named for its folder, which the build tree mirrors.
  static void host(VP::Call &call, bool fresh) {
    std::filesystem::path folder(call.arguments().front());
    if (folder.filename().empty())
      folder = folder.parent_path();
    const bool exists = holds(call.files(), folder, manifest);
    if (fresh && exists)
      throw std::runtime_error(
          std::format("{} holds a view already; view load hosts it", folder.string()));
    if (!fresh && !exists)
      throw std::runtime_error(std::format(
          "{} holds no {}; view new starts a view there", folder.string(), manifest));
    const std::string name = folder.filename().string();
    call.commands().send(
        std::format(": child add {} {}", name, (folder / manifest).string()));
    if (fresh)
      call.commands().send(std::format("{}: view save", name));
  }

  std::filesystem::path _library;
  VP::Command _list;
  VP::Command _drop;
  VP::Command _new;
  VP::Command _load;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<Library>("Library");
}
