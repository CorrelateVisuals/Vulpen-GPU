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
// the module was built against.
constexpr const char *module_entry = "vp_module_" VP_MODULE_STAMP;
constexpr const char *module_file = "module" VP_MODULE_SUFFIX;

// What a module registers, each name after a blank.
template <class Registered> std::string names(const Registered &registered) {
  std::string text;
  for (const auto &entry : registered)
    text += " " + entry.first;
  return text;
}

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
  const auto &registered = entered(module, folder);
  const auto found = registered.find(name);
  if (found != registered.end())
    return found->second();
  throw std::runtime_error(
      std::format("the C++ of {} registers no operator {}; it registers:{}",
                  module,
                  name,
                  registered.empty() ? " nothing" : names(registered)));
}

std::string Modules::sole(const std::string &module,
                          const std::filesystem::path &folder) {
  const auto &registered = entered(module, folder);
  if (registered.size() == 1)
    return registered.begin()->first;
  throw std::runtime_error(
      std::format("the C++ of {} registers{}, so its node names the one it runs, as "
                  "operator = <name>; VP_OPERATOR(<class>) registers one",
                  module,
                  registered.empty() ? " no operator" : names(registered)));
}

const Modules::Registered &Modules::entered(const std::string &module,
                                            const std::filesystem::path &folder) {
  if (!_operators.contains(module))
    enter(module, folder);
  return _operators.at(module);
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

void Modules::builds_ended(std::span<const std::string> built,
                           std::span<const std::string> failed) {
  std::erase_if(_failed, [&](const std::string &view) {
    return std::ranges::find(built, view) != built.end();
  });
  _failed.insert(_failed.end(), failed.begin(), failed.end());
}

bool Modules::build_failed(std::string_view view) const {
  return std::ranges::find(_failed, view) != _failed.end();
}

bool Modules::builds_loadable() {
  const std::string stamp =
      Files::text(Files::executable().parent_path() / VP_MODULE_STAMP_FILE);
  return stamp.substr(0, stamp.find_last_not_of(" \r\n") + 1) == VP_MODULE_STAMP;
}

bool Modules::built(const std::string &module, const std::filesystem::path &folder) const {
  std::error_code unread;
  return std::ranges::any_of(linked_modules,
                             [&](const LinkedModule &linked) { return linked.module == module; }) ||
         std::filesystem::exists(folder / module_file, unread);
}

// Its entry goes too, so a module no node loads again is not named rewritten by every
// later swap, as its file stays newer than the time the entry kept.
void Modules::unload(const std::string &module) {
  _operators.erase(module);
  std::erase_if(_loaded, [&](const Loaded &loaded) { return loaded.module == module; });
}

void Modules::add(std::string_view name, Make maker) {
  if (!_operators.at(_entering).emplace(std::string(name), maker).second)
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
    // The build tree's headers are this vulpen's own, so the module's are older, and a
    // build makes it again; else the headers changed since this vulpen was built.
    if (builds_loadable())
      throw Outdated(std::format("the C++ of {} was built against older engine headers "
                                 "than this vulpen's: a build makes it again",
                                 loaded.module));
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
