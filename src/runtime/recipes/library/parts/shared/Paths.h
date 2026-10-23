#pragma once

#include "runtime/Operator.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// What a folder holds, as the file port lists it; nothing for one it cannot list, as a
// folder its owner keeps closed to others, which a part looking through folders passes
// as it passes a file it cannot read.
inline std::vector<std::string> names_in(VP::FilePort &files, const std::string &folder) {
  try {
    return files.list(folder);
  } catch (const std::runtime_error &) {
    return {};
  }
}

// The names in each folder looked in, each folder listed once, kept by the part that
// looks, as the relations part keeps them between the frames a reading takes, so where a
// file's names lead is found alike by every part that follows them (RV09).
class Listed {
public:
  Listed(VP::FilePort &files, std::map<std::string, std::vector<std::string>> &names)
      : _files(files), _names(names) {}

  // What a folder holds, as names_in gives it.
  const std::vector<std::string> &names(const std::string &folder) {
    auto found = _names.find(folder);
    if (found == _names.end())
      found = _names.emplace(folder, names_in(_files, folder)).first;
    return found->second;
  }

  // Whether a file or a folder is there.
  bool there(const std::filesystem::path &path) {
    const std::vector<std::string> &held = names(path.parent_path().string());
    const std::string name = path.filename().string();
    return std::ranges::find(held, name) != held.end() ||
           std::ranges::find(held, name + '/') != held.end();
  }

private:
  VP::FilePort &_files;
  std::map<std::string, std::vector<std::string>> &_names;
};

// Where a name a file says leads: from the file's folder, or, where it climbs, from a
// folder holding it, as a compiler's include path and CMake's source folder reach; empty
// for nowhere.
inline std::string resolved(Listed &listed,
                            const std::filesystem::path &folder,
                            std::string_view name,
                            bool climbs) {
  while (name.ends_with('/'))
    name.remove_suffix(1);
  if (name.empty())
    return {};
  for (std::filesystem::path from = folder;; from = from.parent_path()) {
    std::filesystem::path path = (from / std::string(name)).lexically_normal();
    if (!path.has_filename()) // a/b/.. is a/, the folder a
      path = path.parent_path();
    if (listed.there(path))
      return path.generic_string();
    if (!climbs || from == from.parent_path())
      return {};
  }
}

// A recipe a manifest's node uses, by its name, in the library's parts or components,
// from a folder holding the manifest; a drop's copy, named with an @, is the view's own.
inline std::string
recipe(Listed &listed, const std::filesystem::path &folder, std::string_view name) {
  if (name.find('@') != std::string_view::npos)
    return {};
  for (const std::string_view kind :
       {"parts/", "components/", "library/parts/", "library/components/"})
    if (std::string found =
            resolved(listed, folder, std::string(kind) + std::string(name), true);
        !found.empty())
      return found;
  return {};
}

} // namespace
