#include "runtime/Modules.h"

#include "baseclasses/Platform.h"
#include "linked-modules.h"

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
constexpr const char *module_entry = "vp_module_" VP_MODULE_STAMP;
constexpr const char *module_file = "module" VP_MODULE_SUFFIX;

} // namespace

struct Modules::Loaded {
  std::string module;
  std::filesystem::path file;
  std::filesystem::file_time_type built;
  std::optional<Library> library;
};

Modules::Modules() = default;

Modules::~Modules() = default;

std::unique_ptr<Operator> Modules::make(const std::string &module,
                                        const std::filesystem::path &folder,
                                        std::string_view name) {
  if (!_operators.contains(module))
    enter(module, folder);
  const auto &registered = _operators.at(module);
  const auto found = registered.find(name);
  if (found != registered.end())
    return found->second();
  std::string names;
  for (const auto &entry : registered)
    names += " " + entry.first;
  throw std::runtime_error(
      std::format("the C++ of {} registers no operator {}; it registers:{}",
                  module,
                  name,
                  names.empty() ? " nothing" : names));
}

std::vector<std::string> Modules::rewritten() const {
  std::vector<std::string> modules;
  for (const Loaded &loaded : _loaded) {
    std::error_code missing;
    const auto built = std::filesystem::last_write_time(loaded.file, missing);
    if (!missing && built != loaded.built)
      modules.push_back(loaded.module);
  }
  return modules;
}

void Modules::unload(const std::string &module) {
  _operators.erase(module);
  std::ranges::find(_loaded, module, &Loaded::module)->library.reset();
}

void Modules::add(std::string_view name, Make make) {
  if (!_operators.at(_entering).emplace(std::string(name), make).second)
    throw std::runtime_error(
        std::format("the C++ of {} registers {} twice", _entering, name));
}

void Modules::enter(const std::string &module, const std::filesystem::path &folder) {
  for (const LinkedModule &linked : linked_modules)
    if (linked.module == module)
      return run(module, linked.entry);
  const auto found = std::ranges::find(_loaded, module, &Loaded::module);
  load(found != _loaded.end() ? *found
                              : _loaded.emplace_back(Loaded{
                                    .module = module, .file = folder / module_file}));
}

void Modules::load(Loaded &loaded) {
  std::error_code missing;
  loaded.built = std::filesystem::last_write_time(loaded.file, missing);
  if (missing)
    throw std::runtime_error(std::format(
        "{} has no C++ built: {} is missing", loaded.module, loaded.file.string()));
  // A module the OS still maps would come back from dlopen with its old code.
  if (Library::mapped(loaded.file))
    throw std::runtime_error(
        std::format("{}: its old module stayed loaded, so loading it "
                    "again would run the old code; restart vulpen",
                    loaded.module));
  loaded.library.emplace(loaded.file);
  void *const symbol = loaded.library->symbol(module_entry);
  if (!symbol) {
    loaded.library.reset();
    throw std::runtime_error(std::format("the C++ of {} was built against other engine "
                                         "headers than this vulpen; restart vulpen",
                                         loaded.module));
  }
  Entry entry = nullptr;
  std::memcpy(&entry, &symbol, sizeof entry); // how POSIX turns a symbol into a function
  run(loaded.module, entry);
}

void Modules::run(const std::string &module, Entry entry) {
  _operators[module]; // a module that registers nothing still counts as entered
  _entering = module;
  entry(*this);
  _entering.clear();
}

} // namespace VP
