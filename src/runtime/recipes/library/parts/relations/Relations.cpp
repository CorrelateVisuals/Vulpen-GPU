#include "Relations.h"

#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t every = 120;      // frames between readings of what is small
constexpr std::size_t small = 1 << 20;    // bytes a reading reads, at most, to be small
constexpr std::size_t largest = 1 << 19;  // bytes of a file read for its lines, at most
constexpr std::size_t folder_room = 2048; // folders of a tree read, at most
constexpr std::string_view blanks = " \t\r";

// What a file's lines say links it to another, as its name or its extension tells.
enum class Language : std::uint8_t { none, code, python, cmake, markdown, shell, manifest };

Language language_of(const std::filesystem::path &file) {
  static constexpr auto code = std::to_array<std::string_view>(
      {".h", ".hpp", ".c", ".cpp", ".cc", ".inl", ".glsl", ".vert", ".frag", ".comp"});
  const std::string extension = file.extension().string();
  if (file.filename() == "CMakeLists.txt" || extension == ".cmake")
    return Language::cmake;
  if (extension == ".vlp")
    return Language::manifest;
  if (extension == ".py")
    return Language::python;
  if (extension == ".md")
    return Language::markdown;
  if (extension == ".sh")
    return Language::shell;
  return std::ranges::find(code, extension) != code.end() ? Language::code : Language::none;
}

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// The names in each folder looked in during a reading, each folder listed once.
class Listed {
public:
  explicit Listed(VP::FilePort &files) : _files(files) {}

  // Whether a file or a folder is there.
  bool there(const std::filesystem::path &path) {
    const std::string folder = path.parent_path().string();
    auto found = _names.find(folder);
    if (found == _names.end())
      found = _names.emplace(folder, _files.list(folder)).first;
    const std::string name = path.filename().string();
    return std::ranges::find(found->second, name) != found->second.end() ||
           std::ranges::find(found->second, name + '/') != found->second.end();
  }

private:
  VP::FilePort &_files;
  std::map<std::string, std::vector<std::string>> _names;
};

// Where a name a file says leads: from the file's folder, or, where it climbs, from a
// folder holding it, as a compiler's include path and CMake's source folder reach; empty
// for nowhere.
std::string resolved(Listed &listed,
                     const std::filesystem::path &folder,
                     std::string_view name,
                     bool climbs) {
  while (name.ends_with('/'))
    name.remove_suffix(1);
  if (name.empty())
    return {};
  for (std::filesystem::path from = folder;; from = from.parent_path()) {
    const std::filesystem::path path = (from / std::string(name)).lexically_normal();
    if (listed.there(path))
      return path.generic_string();
    if (!climbs || from == from.parent_path())
      return {};
  }
}

// An id as a document defines it and a file cites it: capitals, then two digits or
// three, as RV00 or CPP12.
std::size_t id_length(std::string_view text, std::size_t at) {
  std::size_t end = at;
  while (end < text.size() && text[end] >= 'A' && text[end] <= 'Z')
    ++end;
  const std::size_t letters = end - at;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9')
    ++end;
  const std::size_t digits = end - at - letters;
  const bool word = end == text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
  return letters > 0 && letters <= 5 && (digits == 2 || digits == 3) && word ? end - at : 0;
}

// The ids a document defines, each to the document: a bullet that starts with one in
// bold, as - **RV00** or - **V05 Features are recipes.**
void define(std::string_view text,
            const std::string &document,
            std::map<std::string, std::string, std::less<>> &ids) {
  constexpr std::string_view bullet = "- **";
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    if (const std::string_view line = trim(text.substr(at, end - at)); line.starts_with(bullet))
      if (const std::size_t length = id_length(line, bullet.size()); length > 0)
        ids.try_emplace(std::string(line.substr(bullet.size(), length)), document);
    at = end + 1;
  }
}

// The relations one line of a file says, by the file's language.
void line_relations(Listed &listed,
                    const std::filesystem::path &folder,
                    Language language,
                    std::string_view line,
                    const std::function<void(std::string, VP_VIEW::Layer)> &add);

// What a file links to: the files and folders its lines name, by its language, and the
// documents defining the ids it cites. Itself it never links to.
void find(Listed &listed,
          const std::filesystem::path &file,
          std::string_view text,
          const std::map<std::string, std::string, std::less<>> &ids,
          std::vector<VP_VIEW::Relation> &relations) {
  const std::string from = file.generic_string();
  const auto add = [&](std::string to, VP_VIEW::Layer layer) {
    if (!to.empty() && to != from)
      relations.push_back({.from = from, .to = std::move(to), .layer = layer});
  };
  const Language language = language_of(file);
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    line_relations(listed, file.parent_path(), language, text.substr(at, end - at), add);
    at = end + 1;
  }
  for (std::size_t at = 0; at < text.size(); ++at)
    if (const std::size_t length = id_length(text, at);
        length > 0 && (at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1])))) {
      if (const auto found = ids.find(text.substr(at, length)); found != ids.end())
        add(found->second, VP_VIEW::Layer::docs);
      at += length;
    }
}

// A Python module as a path: a.b as a/b, its leading dots counting folders up from the
// file's, as a relative import does; a module's file, else a package's folder.
std::string module(Listed &listed, const std::filesystem::path &folder, std::string_view name) {
  std::filesystem::path from = folder;
  const std::size_t dots = std::min(name.find_first_not_of('.'), name.size());
  for (std::size_t up = 1; up < dots; ++up)
    from = from.parent_path();
  std::string path(name.substr(dots));
  std::ranges::replace(path, '.', '/');
  const std::string file = resolved(listed, from, path + ".py", dots == 0);
  return file.empty() ? resolved(listed, from, path, dots == 0) : file;
}

// A recipe a manifest's node uses, by its name, in the library's parts or components,
// from a folder holding the manifest; a drop's copy, named with an @, is the view's own.
std::string recipe(Listed &listed, const std::filesystem::path &folder, std::string_view name) {
  if (name.find('@') != std::string_view::npos)
    return {};
  for (const std::string_view kind : {"parts/", "components/", "library/parts/",
                                      "library/components/"})
    if (std::string found = resolved(listed, folder, std::string(kind) + std::string(name), true);
        !found.empty())
      return found;
  return {};
}

// The words of a CMake file's line that name a file or a folder, past the variables
// before them, as ${tests}/Barriers.cpp names Barriers.cpp beside the file.
void cmake_line(Listed &listed,
                const std::filesystem::path &folder,
                std::string_view line,
                const std::function<void(std::string, VP_VIEW::Layer)> &add) {
  constexpr std::string_view breaks = " \t\r()\"";
  for (std::size_t at = line.find_first_not_of(breaks); at != std::string_view::npos;) {
    const std::size_t end = std::min(line.find_first_of(breaks, at), line.size());
    std::string_view word = line.substr(at, end - at);
    if (const std::size_t variable = word.rfind('}'); variable != std::string_view::npos)
      word.remove_prefix(variable + 1);
    while (word.starts_with('/'))
      word.remove_prefix(1);
    if (!word.starts_with('-') && word.find_first_of("$<>#") == std::string_view::npos &&
        word.find_first_of("./") != std::string_view::npos)
      add(resolved(listed, folder, word, true), VP_VIEW::Layer::build);
    at = line.find_first_not_of(breaks, end);
  }
}

// The links of a document's line to files, as [text](path), past the web's and the
// anchors within it.
void markdown_line(Listed &listed,
                   const std::filesystem::path &folder,
                   std::string_view line,
                   const std::function<void(std::string, VP_VIEW::Layer)> &add) {
  for (std::size_t at = line.find("]("); at != std::string_view::npos;
       at = line.find("](", at + 2)) {
    const std::size_t end = line.find(')', at + 2);
    if (end == std::string_view::npos)
      return;
    std::string_view target = line.substr(at + 2, end - at - 2);
    target = target.substr(0, target.find('#'));
    if (!target.starts_with("http") && !target.starts_with("mailto:"))
      add(resolved(listed, folder, target, false), VP_VIEW::Layer::docs);
  }
}

void line_relations(Listed &listed,
                    const std::filesystem::path &folder,
                    Language language,
                    std::string_view line,
                    const std::function<void(std::string, VP_VIEW::Layer)> &add) {
  const std::string_view said = trim(line);
  const auto after = [&](std::string_view start) {
    return said.starts_with(start) ? trim(said.substr(start.size())) : std::string_view();
  };
  if (language == Language::code && said.starts_with("#include")) {
    const std::string_view name = after("#include");
    if (name.starts_with('"'))
      add(resolved(listed, folder, name.substr(1, name.find('"', 1) - 1), true),
          VP_VIEW::Layer::code);
  } else if (language == Language::python && (said.starts_with("import ") ||
                                              said.starts_with("from "))) {
    const std::string_view names = said.starts_with("from ") ? after("from") : after("import");
    add(module(listed, folder, names.substr(0, names.find_first_of(" ,"))),
        VP_VIEW::Layer::code);
  } else if (language == Language::shell && (said.starts_with("source ") ||
                                             said.starts_with(". "))) {
    std::string_view name = said.starts_with(". ") ? after(".") : after("source");
    name = name.substr(0, name.find_first_of(blanks));
    add(resolved(listed, folder, name, false), VP_VIEW::Layer::code);
  } else if (language == Language::manifest && said.starts_with("recipe")) {
    if (const std::size_t equals = said.find('='); equals != std::string_view::npos)
      add(recipe(listed, folder, trim(said.substr(equals + 1))), VP_VIEW::Layer::structure);
  } else if (language == Language::cmake) {
    cmake_line(listed, folder, said.substr(0, said.find('#')), add);
  } else if (language == Language::markdown) {
    markdown_line(listed, folder, said, add);
  }
}

// The folder opened as a view's nodes, as the graph shows it: a node each folder in it,
// all the way down, named by its path from the folder holding it, joined by /, holding
// its files; the hidden ones left out, as a manifest leaves them, and folder_room
// folders at most.
VP::View tree_of(VP::FilePort &files, const std::filesystem::path &opened) {
  VP::View tree;
  const std::filesystem::path base = opened.parent_path();
  std::vector<std::filesystem::path> folders{opened};
  for (std::size_t at = 0; at < folders.size() && at < folder_room; ++at) {
    VP::Node node{.name = folders[at].lexically_relative(base).generic_string(),
                  .folder = folders[at]};
    for (const std::string &name : files.list(folders[at].string())) {
      if (name.starts_with('.'))
        continue;
      if (name.ends_with('/'))
        folders.push_back(folders[at] / name.substr(0, name.size() - 1));
      else
        node.files.push_back(name);
    }
    tree.nodes.push_back(std::move(node));
  }
  return tree;
}

bool same(const VP::View &a, const VP::View &b) {
  return std::ranges::equal(a.nodes, b.nodes, [](const VP::Node &one, const VP::Node &two) {
    return one.name == two.name && one.files == two.files;
  });
}

} // namespace

void Relations::bind(VP::Bind &node) {
  _relations = &node.output<VP_VIEW::Relations>("relations");
  _relations->node = node.name();
  _relations->view = node.view_name();
  // As open names it, a / after it or not.
  if (node.sets("folder")) {
    std::filesystem::path folder =
        std::filesystem::path(node.param<std::string>("folder")).lexically_normal();
    _folder = (folder.filename().empty() ? folder.parent_path() : folder).string();
  } else {
    _folder.clear();
  }
  _docs = (std::filesystem::path(node.folder()) / node.param<std::string>("docs"))
              .lexically_normal()
              .string();
  _stale = true;
}

// Read again as the folder or the view shown changes, as an edit replaces the view, and
// every so often while the last reading was small, so a file changed on disk shows.
void Relations::cook(VP::Cook &frame) {
  const std::string shown(frame.commands().current());
  const VP::View *const view = _folder.empty() ? frame.unfolded(shown) : nullptr;
  const bool changed = _stale || view != _read_view || shown != _shown;
  if (!changed && (++_frames < every || _bytes > small))
    return;
  _stale = false;
  _read_view = view;
  _shown = shown;
  _frames = 0;
  _bytes = 0;
  VP::FilePort &files = frame.files();
  Listed listed(files);
  VP::View tree = _folder.empty() ? VP::View{} : tree_of(files, _folder);
  std::vector<std::filesystem::path> sources;
  for (const VP::Node &node : _folder.empty() && view ? view->nodes : tree.nodes)
    for (const std::string &file : node.files)
      if (language_of(file) != Language::none)
        sources.push_back(node.folder / file);
  std::ranges::sort(sources); // a recipe's file, once however many nodes use it
  sources.erase(std::ranges::unique(sources).begin(), sources.end());
  std::map<std::string, std::string, std::less<>> ids;
  for (const std::string &name : files.list(_docs))
    if (name.ends_with(".md"))
      define(files.read((std::filesystem::path(_docs) / name).string()),
             (std::filesystem::path(_docs) / name).generic_string(),
             ids);
  std::vector<VP_VIEW::Relation> relations;
  for (const std::filesystem::path &source : sources) {
    std::string text;
    try {
      text = files.read(source.string());
    } catch (const std::runtime_error &) {
      continue; // gone since it was listed
    }
    _bytes += text.size();
    if (text.size() <= largest)
      find(listed, source, text, ids, relations);
  }
  std::ranges::sort(relations);
  relations.erase(std::ranges::unique(relations).begin(), relations.end());
  if (relations == _relations->relations && _folder == _relations->folder &&
      same(tree, _relations->tree))
    return;
  _relations->folder = _folder;
  _relations->tree = std::move(tree);
  _relations->relations = std::move(relations);
  ++_relations->changes;
}

VP_OPERATORS(registry) {
  registry.add<Relations>("Relations");
}
