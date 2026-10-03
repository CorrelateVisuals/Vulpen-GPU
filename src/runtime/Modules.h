#pragma once

#include "runtime/Operator.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

// A module a release build links in, by "view/folder", and the entry that registers it.
struct LinkedModule {
  std::string_view module;
  void (*entry)(Registry &);
};

// The C++ of every node's folder, which builds as one module (RV08): each registers its
// operators by the names a manifest's operator word uses.
//
// A dev build loads each folder's module from the build tree, and can swap it; a release
// build finds the same code linked in.
class Modules final : public Registry {
public:
  Modules();
  ~Modules();
  Modules(const Modules &) = delete;
  Modules &operator=(const Modules &) = delete;

  // module: "view/folder", the folder from the view's root as the build tree mirrors it;
  // folder: where the build put the module. Throws naming what the module does register
  // when it lacks the class (A02).
  std::unique_ptr<Operator> make(const std::string &module,
                                 const std::filesystem::path &folder,
                                 std::string_view name);
  // Modules, as "view/folder", that the build rewrote since they loaded.
  std::vector<std::string> rewritten() const;
  // The next make() loads the rebuilt module. The caller has destroyed the module's
  // operators first: their code is in it.
  void unload(const std::string &module);

private:
  // A module of a dev build. It holds the platform's Library, so Modules.cpp defines it
  // and this header includes no platform code.
  struct Loaded;

  void add(std::string_view name, Make make) override;
  void enter(const std::string &module, const std::filesystem::path &folder);
  void load(Loaded &loaded);
  void run(const std::string &module, void (*entry)(Registry &));

  std::vector<Loaded> _loaded;
  std::map<std::string, std::map<std::string, Make, std::less<>>, std::less<>> _operators;
  std::string _entering;
};

} // namespace VP
