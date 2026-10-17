#include "Relations.h"

#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t every = 120;     // frames between readings of what is small
constexpr std::size_t largest = 1 << 19; // bytes of a file read for its lines, at most
// What a reading costs, in bytes of files read, a folder listed counting as a page's:
// what a frame reads, at most, past a reading's first, so no frame waits on a reading
// whole (VK02), about a millisecond's; what the first reads, at most, so a small
// reading's curves come with the boxes, and one that small is read again every two
// seconds; and what one reads at all, so its memory stays bounded (A03).
constexpr std::size_t listing = 1 << 12;
constexpr std::size_t budget = 1 << 16;
constexpr std::size_t small = 1 << 20;
constexpr std::size_t reading_room = 1 << 26;
constexpr std::size_t folder_room = 8192; // folders a reading walks, at most
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

// The names in each folder looked in during a reading, each folder listed once, kept in
// the part between the frames a reading takes.
class Listed {
public:
  Listed(VP::FilePort &files, std::map<std::string, std::vector<std::string>> &names)
      : _files(files), _names(names) {}

  // What a folder holds, as the file port lists it.
  const std::vector<std::string> &names(const std::string &folder) {
    auto found = _names.find(folder);
    if (found == _names.end())
      found = _names.emplace(folder, _files.list(folder)).first;
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

// Where a line's relation goes, by the layer it is drawn in, and what in the line names
// it.
using Add = std::function<void(std::string, VP_VIEW::Layer, std::string_view)>;

// The relations one line of a file says, by the file's language.
void line_relations(Listed &listed,
                    const std::filesystem::path &folder,
                    Language language,
                    std::string_view line,
                    const Add &add);

// The documents defining the ids a line cites.
void cite(std::string_view line,
          const std::map<std::string, std::string, std::less<>> &ids,
          const Add &add) {
  for (std::size_t at = 0; at < line.size(); ++at)
    if (const std::size_t length = id_length(line, at);
        length > 0 && (at == 0 || !std::isalnum(static_cast<unsigned char>(line[at - 1])))) {
      if (const auto found = ids.find(line.substr(at, length)); found != ids.end())
        add(found->second, VP_VIEW::Layer::docs, line.substr(at, length));
      at += length;
    }
}

// What a file links to, line by line: the files and folders its lines name, by its
// language, and the documents defining the ids it cites. Itself it never links to.
void find(Listed &listed,
          const std::filesystem::path &file,
          std::string_view text,
          const std::map<std::string, std::string, std::less<>> &ids,
          std::vector<VP_VIEW::Relation> &relations) {
  const std::string from = file.generic_string();
  std::uint32_t line = 0;
  const Add add = [&](std::string to, VP_VIEW::Layer layer, std::string_view said) {
    if (!to.empty() && to != from)
      relations.push_back({.from = from,
                           .to = std::move(to),
                           .layer = layer,
                           .line = line,
                           .said = std::string(said)});
  };
  const Language language = language_of(file);
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    ++line;
    line_relations(listed, file.parent_path(), language, text.substr(at, end - at), add);
    cite(text.substr(at, end - at), ids, add);
    at = end + 1;
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
                const Add &add) {
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
      add(resolved(listed, folder, word, true), VP_VIEW::Layer::build, word);
    at = line.find_first_not_of(breaks, end);
  }
}

// The links of a document's line to files, as [text](path), past the web's and the
// anchors within it.
void markdown_line(Listed &listed,
                   const std::filesystem::path &folder,
                   std::string_view line,
                   const Add &add) {
  for (std::size_t at = line.find("]("); at != std::string_view::npos;
       at = line.find("](", at + 2)) {
    const std::size_t end = line.find(')', at + 2);
    if (end == std::string_view::npos)
      return;
    std::string_view target = line.substr(at + 2, end - at - 2);
    target = target.substr(0, target.find('#'));
    if (!target.starts_with("http") && !target.starts_with("mailto:"))
      add(resolved(listed, folder, target, false), VP_VIEW::Layer::docs, target);
  }
}

void line_relations(Listed &listed,
                    const std::filesystem::path &folder,
                    Language language,
                    std::string_view line,
                    const Add &add) {
  const std::string_view said = trim(line);
  const auto after = [&](std::string_view start) {
    return said.starts_with(start) ? trim(said.substr(start.size())) : std::string_view();
  };
  if (language == Language::code && said.starts_with("#include")) {
    const std::string_view name = after("#include");
    if (name.starts_with('"')) {
      const std::string_view included = name.substr(1, name.find('"', 1) - 1);
      add(resolved(listed, folder, included, true), VP_VIEW::Layer::code, included);
    }
  } else if (language == Language::python && (said.starts_with("import ") ||
                                              said.starts_with("from "))) {
    const std::string_view names = said.starts_with("from ") ? after("from") : after("import");
    const std::string_view imported = names.substr(0, names.find_first_of(" ,"));
    add(module(listed, folder, imported), VP_VIEW::Layer::code, imported);
  } else if (language == Language::shell && (said.starts_with("source ") ||
                                             said.starts_with(". "))) {
    std::string_view name = said.starts_with(". ") ? after(".") : after("source");
    name = name.substr(0, name.find_first_of(blanks));
    add(resolved(listed, folder, name, false), VP_VIEW::Layer::code, name);
  } else if (language == Language::manifest && said.starts_with("recipe")) {
    if (const std::size_t equals = said.find('='); equals != std::string_view::npos) {
      const std::string_view used = trim(said.substr(equals + 1));
      add(recipe(listed, folder, used), VP_VIEW::Layer::structure, used);
    }
  } else if (language == Language::cmake) {
    cmake_line(listed, folder, said.substr(0, said.find('#')), add);
  } else if (language == Language::markdown) {
    markdown_line(listed, folder, said, add);
  }
}

// The folder shown as a view's nodes, as the graph shows its inside: a node for it and
// one each folder in it, holding their files, and one each folder in those, which their
// boxes list; named by its path from base, joined by /, the folder holding its
// repository's top, so a folder keeps its name whichever level shows it, as the graph
// keeps its place by it. Hidden ones too, as .git, so a repository shows whole.
VP::View tree_of(VP::FilePort &files,
                 const std::filesystem::path &shown,
                 const std::filesystem::path &base) {
  VP::View tree;
  std::vector<std::filesystem::path> level{shown};
  for (const bool listed : {true, true, false}) {
    std::vector<std::filesystem::path> inner;
    for (const std::filesystem::path &folder : level) {
      VP::Node node{.name = folder.lexically_relative(base).generic_string(),
                    .folder = folder};
      for (const std::string &name : listed ? files.list(folder.string())
                                            : std::vector<std::string>{})
        if (name.ends_with('/'))
          inner.push_back(folder / name.substr(0, name.size() - 1));
        else
          node.files.push_back(name);
      tree.nodes.push_back(std::move(node));
    }
    level = std::move(inner);
  }
  return tree;
}

// The folder going up from the one shown stops at: the nearest holding .git, the top of
// its repository; outside one, the one shown.
std::string top_of(VP::FilePort &files, const std::filesystem::path &shown) {
  for (std::filesystem::path folder = shown;; folder = folder.parent_path()) {
    const std::vector<std::string> names = files.list(folder.string());
    if (std::ranges::find(names, ".git/") != names.end() ||
        std::ranges::find(names, ".git") != names.end())
      return folder.string();
    if (folder == folder.parent_path())
      return shown.string();
  }
}

bool same(const VP::View &a, const VP::View &b) {
  return std::ranges::equal(a.nodes, b.nodes, [](const VP::Node &one, const VP::Node &two) {
    return one.name == two.name && one.files == two.files;
  });
}

} // namespace

// A rebuild binds the part again, as any edit of its view does, as a file opened in the
// editor sets its param; the reading goes on, and what it read stays, unless the folder
// or the documents to read changed, so a big folder's curves do not go while it is read
// again.
void Relations::bind(VP::Bind &node) {
  _relations = &node.output<VP_VIEW::Relations>("relations");
  _relations->node = node.name();
  _relations->view = node.view_name();
  // As open names it, a / after it or not.
  std::string folder;
  if (node.sets("folder")) {
    const std::filesystem::path named =
        std::filesystem::path(node.param<std::string>("folder")).lexically_normal();
    folder = (named.filename().empty() ? named.parent_path() : named).string();
  }
  std::string docs = (std::filesystem::path(node.folder()) / node.param<std::string>("docs"))
                         .lexically_normal()
                         .string();
  if (folder != _folder || docs != _docs)
    _stale = true;
  _folder = std::move(folder);
  _docs = std::move(docs);
}

// Read as the folder or the view shown changes, as an edit replaces the view: the folder
// at once, so the graph shows its boxes, and the relations as much as a small reading
// reads, so they come with the boxes, and the rest a little a frame; and again every so
// often while the last reading was small, a little a frame, so a file changed on disk
// shows. No frame waits on a large reading whole (VK02).
void Relations::cook(VP::Cook &frame) {
  const std::string shown(frame.commands().current());
  const VP::View *const view = _folder.empty() ? frame.unfolded(shown) : nullptr;
  if (_stale || view != _read_view || shown != _shown) {
    _stale = false;
    _read_view = view;
    _shown = shown;
    start(frame.files(), view);
    if (step(frame.files(), small))
      publish(frame);
    else
      publish_tree();
    return;
  }
  if (!_reading && (++_frames < every || _bytes > small))
    return;
  if (!_reading)
    start(frame.files(), view);
  ++_frames;
  if (step(frame.files(), budget))
    publish(frame);
}

// A reading begun: the folder's tree and the top going up stops at, the folder to walk
// for the files below it, or the files of the view, a recipe's once however many nodes
// use it, and the ids the documents define.
void Relations::start(VP::FilePort &files, const VP::View *view) {
  _reading = true;
  _frames = 0;
  _listed.clear();
  _top = _folder.empty() ? std::string() : top_of(files, _folder);
  _tree = _folder.empty()
              ? VP::View{}
              : tree_of(files, _folder, std::filesystem::path(_top).parent_path());
  _walk.clear();
  _walked = 0;
  _sources.clear();
  if (!_folder.empty())
    _walk.emplace_back(_folder);
  for (std::size_t node = 0; _folder.empty() && view && node < view->nodes.size(); ++node)
    for (const std::string &file : view->nodes[node].files)
      if (language_of(file) != Language::none)
        _sources.push_back(view->nodes[node].folder / file);
  std::ranges::sort(_sources);
  _sources.erase(std::ranges::unique(_sources).begin(), _sources.end());
  _next = 0;
  _read = 0;
  _found.clear();
  _ids.clear();
  for (const std::string &name : files.list(_docs))
    if (name.ends_with(".md"))
      define(files.read((std::filesystem::path(_docs) / name).string()),
             (std::filesystem::path(_docs) / name).generic_string(),
             _ids);
}

// The reading, from where it got, until it has cost a budget: first the folders below
// the folder shown, each listed once, for the files to read, past the hidden ones, which
// hold the state of tools, as .git, and no line read here; then the files. Returns
// whether the reading is done, as it is once it read reading_room bytes.
bool Relations::step(VP::FilePort &files, std::size_t bytes) {
  Listed listed(files, _listed);
  std::size_t taken = 0;
  for (; _walked < _walk.size() && taken < bytes; ++_walked, taken += listing) {
    const std::filesystem::path folder = _walk[_walked];
    _read += listing;
    for (const std::string &name : listed.names(folder.string()))
      if (name.starts_with('.'))
        continue;
      else if (name.ends_with('/') && _walk.size() < folder_room)
        _walk.push_back(folder / name.substr(0, name.size() - 1));
      else if (!name.ends_with('/') && language_of(name) != Language::none)
        _sources.push_back(folder / name);
  }
  for (; _next < _sources.size() && taken < bytes && _read < reading_room; ++_next) {
    std::string text;
    try {
      text = files.read(_sources[_next].string());
    } catch (const std::runtime_error &) {
      continue; // gone since it was listed
    }
    taken += text.size();
    _read += text.size();
    if (text.size() <= largest)
      find(listed, _sources[_next], text, _ids, _found);
  }
  return _walked == _walk.size() && (_next == _sources.size() || _read >= reading_room);
}

// The folder shown, at once, while its relations are still read: no curve, rather than
// those of what was shown before.
void Relations::publish_tree() {
  _relations->folder = _folder;
  _relations->top = _top;
  _relations->tree = _tree;
  _relations->relations.clear();
  ++_relations->changes;
}

// The reading handed to the graph, if it changed anything, a relation once however many
// lines of its file say it, at the first; what it read, and in how many frames, goes to
// the log at debug, so a person sees what a folder costs (C03, C09).
void Relations::publish(VP::Cook &frame) {
  _reading = false;
  // A folder shows its repository, so what lies past its top, as the system's files a
  // build's CMake names, is no relation of it.
  if (!_top.empty())
    std::erase_if(_found, [&](const VP_VIEW::Relation &relation) {
      const std::filesystem::path within =
          std::filesystem::path(relation.to).lexically_relative(_top);
      return within.empty() || *within.begin() == "..";
    });
  std::ranges::sort(_found);
  _found.erase(std::ranges::unique(_found,
                                   {},
                                   [](const VP_VIEW::Relation &relation) {
                                     return std::tie(
                                         relation.from, relation.to, relation.layer);
                                   })
                   .begin(),
               _found.end());
  frame.log(VP::Level::debug,
            std::format("read {} files and {} folders of {}, {} KB in {} frames, "
                        "finding {} relations",
                        _next,
                        _walked,
                        _folder.empty() ? std::format("view {}", _shown) : _folder,
                        _read >> 10,
                        _frames + 1,
                        _found.size()));
  _frames = 0;
  _bytes = _read;
  if (_found == _relations->relations && _folder == _relations->folder &&
      _top == _relations->top && same(_tree, _relations->tree))
    return;
  _relations->folder = _folder;
  _relations->top = _top;
  _relations->tree = std::move(_tree);
  _relations->relations = std::move(_found);
  ++_relations->changes;
}

VP_OPERATORS(registry) {
  registry.add<Relations>("Relations");
}
