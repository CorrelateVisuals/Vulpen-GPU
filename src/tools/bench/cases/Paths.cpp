#include "shared/Paths.h"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <filesystem>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The library's shared/Paths.h: where the names a file says lead, as the relations part
// follows a manifest's recipes and a source's includes. The files are a project and a
// library's parts and components in the sandbox, read through the file port over them,
// which here counts what it lists and refuses a folder its owner keeps closed.

namespace VP::bench {

namespace {

// The sandbox's files through SandboxFiles, but a folder named closed, which it refuses
// to list, as the file port refuses a folder it may not read.
class Watched final : public FilePort {
public:
  std::size_t lists = 0;

  std::string_view text(File file) override {
    return _files.text(file);
  }
  void save(File file, std::string_view text) override {
    _files.save(file, text);
  }
  std::string read(std::string_view file) override {
    return _files.read(file);
  }
  void save(std::string_view file, std::string_view text) override {
    _files.save(file, text);
  }
  void remove(std::string_view file) override {
    _files.remove(file);
  }
  std::vector<std::string> list(std::string_view folder) override {
    ++lists;
    if (std::filesystem::path(folder).filename() == "closed")
      throw std::runtime_error(std::format("{}: permission denied", folder));
    return _files.list(folder);
  }
  void copy(std::string_view from, std::string_view to) override {
    _files.copy(from, to);
  }
  void rename(std::string_view from, std::string_view to) override {
    _files.rename(from, to);
  }
  void save(const View &view, std::string_view file) override {
    _files.save(view, file);
  }
  View manifest(std::string_view file) override {
    return _files.manifest(file);
  }
  void make(std::string_view folder) override {
    _files.make(folder);
  }
  std::span<const Moved> moved() const override {
    return _files.moved();
  }
  std::uint64_t version(File file) override {
    return _files.version(file);
  }

private:
  SandboxFiles _files;
};

// A project beside a library, as a checkout holds them, and a folder closed to others,
// in a folder of their own, as the sandbox's own folders differ by context.
std::filesystem::path planted_tree(const Rows &rows) {
  const std::filesystem::path root = rows.root() / "tree";
  for (const std::string_view file : {"projects/a/view.vlp",
                                      "projects/a/node/Node.frag",
                                      "projects/a/node/Node.glsl",
                                      "projects/a/node/inner/Inner.vert",
                                      "projects/a/contracts/Spin.h",
                                      "library/parts/image/view.vlp",
                                      "library/parts/list/view.vlp",
                                      "library/components/ide/view.vlp",
                                      "projects/a/parts/own/view.vlp",
                                      "closed/secret.txt"})
    Files::save((root / file).string(), "");
  return root;
}

std::string shown_names(const std::vector<std::string> &names, Names &table) {
  return show(names, table);
}

} // namespace

void cases(Bench &bench) {
  bench.check("Paths.h/names_in", [](Rows &rows) {
    const std::filesystem::path root = planted_tree(rows);
    Watched files;
    for (const std::string_view folder :
         {"projects/a", "projects/a/node", "projects/a/view.vlp", "gone", "closed", "."})
      rows.row(std::format(
          "{} -> {}",
          folder,
          shown_names(names_in(files, (root / folder).string()), rows.names())));
  });

  bench.check("Paths.h/Listed::Listed", [](Rows &rows) {
    Watched files;
    std::map<std::string, std::vector<std::string>> names;
    const Listed listed(files, names);
    rows.row(std::format("made: {} folders kept, {} listed", names.size(), files.lists));
  });
  bench.check("Paths.h/Listed::names", [](Rows &rows) {
    const std::filesystem::path root = planted_tree(rows);
    Watched files;
    std::map<std::string, std::vector<std::string>> names;
    Listed listed(files, names);
    for (const std::string_view folder :
         {"projects/a", "projects/a", "gone", "gone", "closed"}) {
      const std::vector<std::string> &held = listed.names((root / folder).string());
      rows.row(std::format("{} -> {}; {} listed, {} kept",
                           folder,
                           shown_names(held, rows.names()),
                           files.lists,
                           names.size()));
    }
  });
  bench.check("Paths.h/Listed::there", [](Rows &rows) {
    const std::filesystem::path root = planted_tree(rows);
    Watched files;
    std::map<std::string, std::vector<std::string>> names;
    Listed listed(files, names);
    for (const std::string_view path : {"projects/a/view.vlp",
                                        "projects/a/node",
                                        "projects/a/node/",
                                        "projects/a/gone",
                                        "gone/a",
                                        "closed/secret.txt",
                                        "projects",
                                        "."})
      rows.row(std::format("{} -> {}", path, listed.there(root / path)));
    rows.row(std::format("{} folders listed", files.lists));
  });

  bench.check("Paths.h/resolved", [](Rows &rows) {
    const std::filesystem::path root = planted_tree(rows);
    Watched files;
    std::map<std::string, std::vector<std::string>> names;
    Listed listed(files, names);
    const std::filesystem::path from = root / "projects/a/node/inner";
    for (const std::string_view name : {"Inner.vert",
                                        "Node.glsl",
                                        "../Node.glsl",
                                        "contracts/Spin.h",
                                        "node",
                                        "node/",
                                        "inner//",
                                        "../../a/view.vlp",
                                        "../../../..",
                                        "gone.h",
                                        "",
                                        "/",
                                        ".",
                                        "..",
                                        "projects/a/view.vlp"})
      for (const bool climbs : {false, true}) {
        const std::string found = resolved(listed, from, name, climbs);
        rows.row(std::format("{}{} -> {}",
                             escaped(name),
                             climbs ? ", climbing" : "",
                             escaped(here(found, root))));
      }
  });

  bench.check("Paths.h/recipe", [](Rows &rows) {
    const std::filesystem::path root = planted_tree(rows);
    Watched files;
    std::map<std::string, std::vector<std::string>> names;
    Listed listed(files, names);
    for (const std::string_view name :
         {"image", "list", "ide", "own", "gone", "image@1a2b", ""})
      rows.row(
          std::format("{} from projects/a -> {}",
                      escaped(name),
                      escaped(here(recipe(listed, root / "projects/a", name), root))));
  });
}

} // namespace VP::bench
