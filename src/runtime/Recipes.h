#pragma once

#include "baseclasses/Platform.h"
#include "runtime/Operator.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

// A recipe a release build links in, by "view/recipe", and the entry that registers it.
struct LinkedRecipe {
  std::string_view recipe;
  void (*entry)(Registry &);
};

// The recipe library. Deploying copies a recipe into the view with every recipe it
// deploys and every contract it includes, and the view owns those copies from then on;
// a later library edit never reaches them.
//
// A dev build loads each deployed recipe's C++ as a module it can swap; a release build
// finds the same code linked in.
class Recipes final : public Registry {
public:
  // recipe: "view/recipe"; folder: where the build put its module. Throws naming what
  // the recipe does register when it lacks the class (A02).
  std::unique_ptr<Operator> make(const std::string &recipe,
                                 const std::filesystem::path &folder,
                                 std::string_view name);
  // Recipes, as "view/recipe", whose module the build rewrote since it loaded.
  std::vector<std::string> rewritten() const;
  // The next make() loads the rebuilt module. The caller has destroyed the recipe's
  // operators first: their code is in the module.
  void unload(const std::string &recipe);

private:
  struct Module {
    std::string recipe;
    std::filesystem::path file;
    std::filesystem::file_time_type built;
    std::optional<Library> library;
  };

  void add(std::string_view name, Make make) override;
  void enter(const std::string &recipe, const std::filesystem::path &folder);
  void load(Module &module);
  void run(const std::string &recipe, void (*entry)(Registry &));

  std::vector<Module> _modules;
  std::map<std::string, std::map<std::string, Make, std::less<>>, std::less<>> _operators;
  std::string _entering;
};

} // namespace VP
