#include "runtime/Recipes.h"

#include "baseclasses/Platform.h"
#include "linked-recipes.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>

namespace VP {

namespace {

using Entry = void (*)(Registry &);
static_assert(sizeof(Entry) == sizeof(void *));

// Every module exports its entry under one name, whose stamp hashes the engine headers
// the module was built against (docs/plans/live-code.md, rule 6).
constexpr const char *module_entry = "vp_recipe_" VP_RECIPE_STAMP;
constexpr const char *module_file = "recipe" VP_MODULE_SUFFIX;

} // namespace

struct Recipes::Module {
  std::string recipe;
  std::filesystem::path file;
  std::filesystem::file_time_type built;
  std::optional<Library> library;
};

Recipes::Recipes() = default;

Recipes::~Recipes() = default;

std::unique_ptr<Operator> Recipes::make(const std::string &recipe,
                                        const std::filesystem::path &folder,
                                        std::string_view name) {
  if (!_operators.contains(recipe))
    enter(recipe, folder);
  const auto &registered = _operators.at(recipe);
  const auto found = registered.find(name);
  if (found != registered.end())
    return found->second();
  std::string names;
  for (const auto &entry : registered)
    names += " " + entry.first;
  throw std::runtime_error(
      std::format("recipe {} registers no operator {}; it registers:{}",
                  recipe,
                  name,
                  names.empty() ? " nothing" : names));
}

std::vector<std::string> Recipes::rewritten() const {
  std::vector<std::string> recipes;
  for (const Module &module : _modules) {
    std::error_code missing;
    const auto built = std::filesystem::last_write_time(module.file, missing);
    if (!missing && built != module.built)
      recipes.push_back(module.recipe);
  }
  return recipes;
}

void Recipes::unload(const std::string &recipe) {
  _operators.erase(recipe);
  std::ranges::find(_modules, recipe, &Module::recipe)->library.reset();
}

void Recipes::add(std::string_view name, Make make) {
  if (!_operators.at(_entering).emplace(std::string(name), make).second)
    throw std::runtime_error(
        std::format("recipe {} registers {} twice", _entering, name));
}

void Recipes::enter(const std::string &recipe, const std::filesystem::path &folder) {
  for (const LinkedRecipe &linked : linked_recipes)
    if (linked.recipe == recipe)
      return run(recipe, linked.entry);
  const auto found = std::ranges::find(_modules, recipe, &Module::recipe);
  load(found != _modules.end() ? *found
                               : _modules.emplace_back(Module{
                                     .recipe = recipe, .file = folder / module_file}));
}

void Recipes::load(Module &module) {
  std::error_code missing;
  module.built = std::filesystem::last_write_time(module.file, missing);
  if (missing)
    throw std::runtime_error(std::format("recipe {} has no C++ built: {} is missing",
                                         module.recipe,
                                         module.file.string()));
  // A module the OS still maps would come back from dlopen with its old code.
  if (Library::mapped(module.file))
    throw std::runtime_error(std::format("recipe {}: its old module stayed loaded, so "
                                         "loading it again would run the old code; "
                                         "restart vulpen",
                                         module.recipe));
  module.library.emplace(module.file);
  void *const symbol = module.library->symbol(module_entry);
  if (!symbol) {
    module.library.reset();
    throw std::runtime_error(
        std::format("recipe {} was built against other engine headers "
                    "than this vulpen; restart vulpen",
                    module.recipe));
  }
  Entry entry = nullptr;
  std::memcpy(&entry, &symbol, sizeof entry); // how POSIX turns a symbol into a function
  run(module.recipe, entry);
}

void Recipes::run(const std::string &recipe, Entry entry) {
  _operators[recipe]; // a recipe that registers nothing still counts as entered
  _entering = recipe;
  entry(*this);
  _entering.clear();
}

} // namespace VP
