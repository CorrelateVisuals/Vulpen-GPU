// The relations part's operator. It runs past 800 lines (CPP12): the readers of each
// language's lines and of the seams between files sit beside the reading that calls them,
// in one file's unnamed namespace, so two copies of the part never clash; those needing
// none of its state could move to a header beside it, as the graph part's do.
#include "Relations.h"

#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t every = 120;     // frames between readings of what is small
constexpr std::size_t largest = 1 << 19; // bytes of a file read for its lines, at most
// What a frame spends on a reading, at most, past its first, so no frame waits on a
// reading whole (VK02), however fast the build reads, and the first, so a small reading's
// curves come with the boxes; between them a file's lines are read so many bytes at a
// time, so a large file spreads over frames too.
constexpr std::chrono::microseconds frame_time{4000};
constexpr std::chrono::microseconds first_time{8000};
constexpr std::size_t slice = 1 << 16;
// What a reading costs, in bytes of files read, a folder listed counting as a page's:
// what one reads to be small, read again every two seconds, and what one reads at all,
// so its memory stays bounded (A03).
constexpr std::size_t listing = 1 << 12;
constexpr std::size_t small = 1 << 20;
constexpr std::size_t reading_room = 1 << 26;
constexpr std::size_t folder_room = 8192; // folders a reading walks, at most
constexpr std::size_t known_room = 8;     // folders whose relations it keeps, at most
// Frames a folder shown anew waits for its relations, at most, so its boxes come with
// their curves, laid out once, as a reading past a small one's first frame takes a few;
// one that takes longer shows the folder then, and its curves as they come.
constexpr std::uint64_t hold = 15;
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

// The documents defining the ids a line cites, each from a capital that starts a word,
// as only one may start an id.
void cite(std::string_view line,
          const std::map<std::string, std::string, std::less<>> &ids,
          const Add &add) {
  constexpr std::string_view capitals = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  if (ids.empty())
    return;
  for (std::size_t at = line.find_first_of(capitals); at != std::string_view::npos;
       at = line.find_first_of(capitals, at + 1)) {
    if (at > 0 && std::isalnum(static_cast<unsigned char>(line[at - 1])))
      continue;
    if (const std::size_t length = id_length(line, at); length > 0) {
      if (const auto found = ids.find(line.substr(at, length)); found != ids.end())
        add(found->second, VP_VIEW::Layer::docs, line.substr(at, length));
      at += length - 1;
    }
  }
}

// What a file links to, line by line: the files and folders its lines name, by its
// language, and the documents defining the ids it cites; of a slice of it, its lines
// numbered past the lines before it. Itself it never links to.
void find(Listed &listed,
          const std::filesystem::path &file,
          std::string_view text,
          std::uint32_t before,
          const std::map<std::string, std::string, std::less<>> &ids,
          std::vector<VP_VIEW::Relation> &relations) {
  const std::string from = file.generic_string();
  std::uint32_t line = before;
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

// The shaders a line of C++ loads by their SPIR-V's name, as "Triangle.vert.spv", which
// the build compiles from Triangle.vert beside it: C++ hands the engine what draws with
// it, so data leaves the C++ for the shader.
void loaded(Listed &listed,
            const std::filesystem::path &folder,
            std::string_view line,
            const Add &add) {
  constexpr std::string_view spirv = ".spv\"";
  for (std::size_t end = line.find(spirv); end != std::string_view::npos;
       end = line.find(spirv, end + spirv.size())) {
    const std::size_t open = end == 0 ? std::string_view::npos : line.rfind('"', end - 1);
    if (open == std::string_view::npos)
      continue;
    add(resolved(listed, folder, line.substr(open + 1, end - open - 1), false),
        VP_VIEW::Layer::engine,
        line.substr(open + 1, end + spirv.size() - open - 2));
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
  } else if (language == Language::code) {
    loaded(listed, folder, said, add);
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

// A file's lines, each with its number, counted from 1, or of a slice of it, past the
// lines before it.
template <class Line>
void each_line(std::string_view text, const Line &line, std::uint32_t before = 0) {
  std::uint32_t number = before;
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    line(text.substr(at, end - at), ++number);
    at = end + 1;
  }
}

bool cpp(const std::filesystem::path &file) {
  static constexpr auto endings =
      std::to_array<std::string_view>({".h", ".hpp", ".cpp", ".cc", ".inl"});
  return std::ranges::find(endings, file.extension().string()) != endings.end();
}

bool shader(const std::filesystem::path &file) {
  static constexpr auto endings = std::to_array<std::string_view>(
      {".glsl", ".vert", ".frag", ".comp", ".tesc", ".tese", ".geom"});
  return std::ranges::find(endings, file.extension().string()) != endings.end();
}

// The stages of a draw in the order its data goes through them, as glslang names them by
// ending; none for a shader that is no stage of a draw.
std::optional<std::size_t> draw_stage(const std::filesystem::path &file) {
  static constexpr auto stages =
      std::to_array<std::string_view>({".vert", ".tesc", ".tese", ".geom", ".frag"});
  const auto found = std::ranges::find(stages, file.extension().string());
  return found == stages.end() ? std::nullopt
                               : std::optional(std::size_t(found - stages.begin()));
}

// What a stage writes out or reads in, a line each: past a layout, whose location it
// keeps, and the words that say how it is interpolated, in or out, a type and a name,
// before an array's size; not a block, nor a built-in, nor a compute shader's in, which
// names no value.
std::vector<Varying> varyings_of(std::string_view text) {
  static constexpr auto qualifiers = std::to_array<std::string_view>(
      {"flat", "smooth", "noperspective", "centroid", "sample", "patch", "invariant"});
  constexpr std::string_view location = "location";
  std::vector<Varying> varyings;
  each_line(text, [&](std::string_view line, std::uint32_t number) {
    line = trim(line.substr(0, line.find("//")));
    if (!line.ends_with(';'))
      return;
    Varying varying{.line = number};
    if (line.starts_with("layout")) {
      const std::size_t close = line.find(')');
      const std::size_t at = line.find(location);
      if (close == std::string_view::npos)
        return;
      if (at != std::string_view::npos && at < close) {
        std::uint32_t value = 0;
        const std::size_t digits = line.find_first_of("0123456789", at);
        if (digits < close &&
            std::from_chars(line.data() + digits, line.data() + close, value).ec ==
                std::errc{})
          varying.location = value;
      }
      line = trim(line.substr(close + 1));
    }
    std::vector<std::string_view> words;
    for (std::size_t at = 0; at < line.size();) {
      const std::size_t end = std::min(line.find_first_of(" \t;[", at), line.size());
      if (end > at)
        words.push_back(line.substr(at, end - at));
      at = line.find_first_not_of(" \t", end);
      if (at == std::string_view::npos || line[at] == ';' || line[at] == '[')
        break;
    }
    std::erase_if(words, [](std::string_view word) {
      return std::ranges::find(qualifiers, word) != qualifiers.end();
    });
    if (words.size() != 3 || (words[0] != "in" && words[0] != "out") ||
        words[2].starts_with("gl_"))
      return;
    varying.out = words[0] == "out";
    varying.name = std::string(words[2]);
    varyings.push_back(std::move(varying));
  });
  return varyings;
}

// A call of the Bind a C++ file names a port by: whether data leaves the C++ through it,
// and whether it reaches the node's shaders' pass block, or a C++ connection.
struct Binding {
  std::string_view call;
  bool sends = false;
  bool pass = false;
};

// The names a C++ file binds: those quoted after the calls of the Bind that reach its
// node's shaders' pass block, value, upload, texture and readback, and those of its C++
// connections, output and input.
void bind_names(std::string_view text,
                const std::filesystem::path &file,
                std::uint32_t before,
                std::vector<Bound> &bound) {
  static constexpr auto calls =
      std::to_array<Binding>({{.call = ".value<", .sends = true, .pass = true},
                              {.call = ".upload<", .sends = true, .pass = true},
                              {.call = ".texture<", .sends = true, .pass = true},
                              {.call = ".readback<", .sends = false, .pass = true},
                              {.call = ".output<", .sends = true, .pass = false},
                              {.call = ".input<", .sends = false, .pass = false}});
  each_line(text, [&](std::string_view line, std::uint32_t number) {
    for (const Binding &binding : calls)
      for (std::size_t at = line.find(binding.call); at != std::string_view::npos;
           at = line.find(binding.call, at + binding.call.size())) {
        const std::size_t open = line.find("(\"", at + binding.call.size());
        const std::size_t close =
            open == std::string_view::npos ? open : line.find('"', open + 2);
        if (close != std::string_view::npos)
          bound.push_back({.file = file,
                           .name = std::string(line.substr(open + 2, close - open - 2)),
                           .line = number,
                           .sends = binding.sends,
                           .pass = binding.pass});
      }
  }, before);
}

// The names a shader's pass block declares, the last word of each member, past its
// comments, before an array's size, each with its line, counted from the file's first.
std::vector<Declared> declared_names(std::string_view text) {
  std::vector<Declared> names;
  const std::size_t pass = text.find("uniform Pass");
  const std::size_t open = text.find('{', pass);
  const std::size_t close = text.find('}', open);
  if (pass == std::string_view::npos || open == std::string_view::npos ||
      close == std::string_view::npos)
    return names;
  const auto before =
      static_cast<std::uint32_t>(std::ranges::count(text.substr(0, open), '\n'));
  const std::string_view block = text.substr(open + 1, close - open - 1);
  each_line(block, [&](std::string_view line, std::uint32_t number) {
    line = trim(line.substr(0, line.find("//")));
    if (!line.ends_with(';'))
      return;
    line = trim(line.substr(0, std::min(line.find('['), line.size() - 1)));
    const std::size_t word = line.find_last_of(" \t*");
    names.push_back(
        {.name = std::string(line.substr(word == std::string_view::npos ? 0 : word + 1)),
         .line = before + number});
  });
  return names;
}

// A manifest's connections, each with the line naming its writer, past its comments.
std::vector<Wired> wired_of(std::string_view text,
                            const std::filesystem::path &manifest) {
  std::vector<Wired> wired;
  bool inside = false;
  each_line(text, [&](std::string_view line, std::uint32_t number) {
    line = trim(line.substr(0, line.find('#')));
    if (line.starts_with('[')) {
      inside = line.starts_with("[connection");
      const std::size_t open = line.find('"');
      const std::size_t close =
          open == std::string_view::npos ? open : line.find('"', open + 1);
      if (inside && close != std::string_view::npos)
        wired.push_back({.manifest = manifest,
                         .name = std::string(line.substr(open + 1, close - open - 1))});
      inside = inside && close != std::string_view::npos;
      return;
    }
    const std::size_t equals = line.find('=');
    if (!inside || equals == std::string_view::npos)
      return;
    const std::string_view key = trim(line.substr(0, equals));
    const std::string_view value = trim(line.substr(equals + 1));
    if (key == "from") {
      wired.back().from = value;
      wired.back().line = number;
    } else if (key == "to") {
      wired.back().to.emplace_back(value);
    }
  });
  return wired;
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
      for (const std::string &name :
           listed ? names_in(files, folder.string()) : std::vector<std::string>{})
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

// A folder a tool keeps its state in, as a build tree, which CMake marks with its cache,
// or a Python environment, which its config marks: as a hidden folder, it holds none of
// the relations of the files a person writes, but many files of its own.
bool tool_state(const std::vector<std::string> &names) {
  static constexpr auto marks =
      std::to_array<std::string_view>({"CMakeCache.txt", "pyvenv.cfg"});
  return std::ranges::any_of(names, [](const std::string &name) {
    return std::ranges::find(marks, name) != marks.end();
  });
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
  _relations->asked = folder;
  _folder = std::move(folder);
  _docs = std::move(docs);
}

// Read as the folder or the view shown changes, as an edit replaces the view: as much as
// first_time reads at once, so a small reading's relations come with the boxes, and the
// rest frame_time a frame; a folder shows with its relations once they are read, for
// hold frames at most, and then alone, as a view's boxes show at once and its curves as
// they come; and again every so often while the last reading was small, a little a
// frame, so a file changed on disk shows. No frame waits on a large reading whole
// (VK02). Only how many frames a reading takes depends on the time; what it finds does
// not (C01).
void Relations::cook(VP::Cook &frame) {
  const auto now = std::chrono::steady_clock::now();
  const std::string shown(frame.commands().current());
  const VP::View *const view = _folder.empty() ? frame.unfolded(shown) : nullptr;
  if (_stale || view != _read_view || shown != _shown) {
    _stale = false;
    _read_view = view;
    _shown = shown;
    start(frame.files(), view);
    _holding = !_folder.empty();
    if (step(frame.files(), now + first_time))
      publish(frame);
    else if (!recall() && !_holding)
      publish_tree();
    return;
  }
  if (!_reading && (++_frames < every || _bytes > small))
    return;
  if (!_reading)
    start(frame.files(), view);
  ++_frames;
  if (step(frame.files(), now + frame_time))
    publish(frame);
  else if (_holding && _frames >= hold)
    publish_tree();
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
  _text.clear();
  _sliced = 0;
  _lines = 0;
  _read = 0;
  _found.clear();
  _ids.clear();
  _bound.clear();
  _declared.clear();
  _varyings.clear();
  _wired.clear();
  for (const std::string &name : files.list(_docs))
    if (name.ends_with(".md"))
      define(files.read((std::filesystem::path(_docs) / name).string()),
             (std::filesystem::path(_docs) / name).generic_string(),
             _ids);
}

// The reading, from where it got, until the time given: first the folders below the
// folder shown, each listed once, for the files to read, past the hidden ones and those a
// tool keeps its state in, which hold the state of tools, as .git and a build tree, and
// no line read here; then the files, slice lines at a time. Returns whether the reading
// is done, as it is once it read reading_room bytes.
bool Relations::step(VP::FilePort &files, std::chrono::steady_clock::time_point until) {
  Listed listed(files, _listed);
  const auto spent = [&] { return std::chrono::steady_clock::now() >= until; };
  for (; _walked < _walk.size() && !spent(); ++_walked) {
    const std::filesystem::path folder = _walk[_walked];
    _read += listing;
    const std::vector<std::string> &names = listed.names(folder.string());
    if (_walked > 0 && tool_state(names))
      continue;
    for (const std::string &name : names)
      if (name.starts_with('.'))
        continue;
      else if (name.ends_with('/') && _walk.size() < folder_room)
        _walk.push_back(folder / name.substr(0, name.size() - 1));
      else if (!name.ends_with('/') && language_of(name) != Language::none)
        _sources.push_back(folder / name);
  }
  while (_next < _sources.size() && _read < reading_room && !spent())
    if (read_slice(files, listed))
      ++_next;
  return _walked == _walk.size() && (_next == _sources.size() || _read >= reading_room);
}

// The next lines of the file the reading is at, a slice of them ending at a line's end,
// read whole once its first slice comes; whether the file is done, as one gone since it
// was listed is, and one larger than largest, whose lines are not read. Once its last
// slice is read, what only the whole file says: a shader's pass block and what its stage
// passes on, and the connections of a manifest.
bool Relations::read_slice(VP::FilePort &files, Listed &listed) {
  const std::filesystem::path &file = _sources[_next];
  if (_sliced == 0) {
    try {
      _text = files.read(file.string());
    } catch (const std::runtime_error &) {
      return true;
    }
    _read += _text.size();
    if (_text.size() > largest)
      return true;
  }
  const std::size_t end =
      std::min(_text.find('\n', std::min(_sliced + slice, _text.size())), _text.size());
  const std::string_view lines = std::string_view(_text).substr(_sliced, end - _sliced);
  find(listed, file, lines, _lines, _ids, _found);
  if (cpp(file))
    bind_names(lines, file, _lines, _bound);
  _lines += static_cast<std::uint32_t>(std::ranges::count(lines, '\n')) + 1;
  _sliced = end + 1;
  if (_sliced < _text.size())
    return false;
  if (shader(file)) {
    _declared.emplace(file, declared_names(_text));
    if (draw_stage(file))
      _varyings.emplace(file, varyings_of(_text));
  } else if (!_folder.empty() && language_of(file) == Language::manifest) {
    std::ranges::move(wired_of(_text, file), std::back_inserter(_wired));
  }
  _sliced = 0;
  _lines = 0;
  return true;
}

// A folder read lately, shown with what was found then, while it is read again, which
// hands them on again only if they changed; whether it was.
bool Relations::recall() {
  const auto known = std::ranges::find(_known, _folder, &Known::folder);
  if (_folder.empty() || known == _known.end())
    return false;
  _holding = false;
  _relations->folder = _folder;
  _relations->top = _top;
  _relations->tree = _tree;
  _relations->relations = known->relations;
  ++_relations->changes;
  return true;
}

// The folder shown, while its relations are still read past the frames it waits for
// them: no curve, rather than those of what was shown before.
void Relations::publish_tree() {
  _holding = false;
  _relations->folder = _folder;
  _relations->top = _top;
  _relations->tree = _tree;
  _relations->relations.clear();
  ++_relations->changes;
}

// The reading handed to the graph, if it changed anything, a relation once for each name
// that says it, at the first line saying it, so a tip lists each include, id or bound
// name once; what it read, and in how many frames, goes to the log at debug, so a person
// sees what a folder costs (C03, C09).
void Relations::publish(VP::Cook &frame) {
  _reading = false;
  _holding = false;
  // A folder shows its repository, so what lies past its top, as the system's files a
  // build's CMake names, is no relation of it.
  if (!_top.empty())
    std::erase_if(_found, [&](const VP_VIEW::Relation &relation) {
      const std::filesystem::path within =
          std::filesystem::path(relation.to).lexically_relative(_top);
      return within.empty() || *within.begin() == "..";
    });
  index_folders();
  relate_seams();
  relate_stages();
  relate_wires();
  _in_folder.clear();
  const auto said = [](const VP_VIEW::Relation &relation) {
    return std::tie(relation.from, relation.to, relation.layer, relation.said);
  };
  std::ranges::sort(_found, {}, [](const VP_VIEW::Relation &relation) {
    return std::tie(
        relation.from, relation.to, relation.layer, relation.said, relation.line);
  });
  _found.erase(std::ranges::unique(_found, {}, said).begin(), _found.end());
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
  if (!_folder.empty()) {
    std::erase_if(_known, [&](const Known &known) { return known.folder == _folder; });
    _known.push_back({.folder = _folder, .relations = _found});
    if (_known.size() > known_room)
      _known.erase(_known.begin());
  }
  if (_found == _relations->relations && _folder == _relations->folder &&
      _top == _relations->top && same(_tree, _relations->tree))
    return;
  _relations->folder = _folder;
  _relations->top = _top;
  _relations->tree = std::move(_tree);
  _relations->relations = std::move(_found);
  ++_relations->changes;
}

// What the files read bind, declare and pass on, by their folders, in the order they
// were read, so each relation below looks in one folder rather than every file read.
void Relations::index_folders() {
  _in_folder.clear();
  for (const Bound &bind : _bound)
    _in_folder[bind.file.parent_path()].bound.push_back(&bind);
  for (const auto &[file, names] : _declared)
    _in_folder[file.parent_path()].declared.emplace_back(&file, &names);
  for (const auto &[file, varyings] : _varyings)
    _in_folder[file.parent_path()].varyings.emplace_back(&file, &varyings);
}

// What C++ binds in its node's shaders' pass block, the engine carries between it and
// the shader of its folder declaring it: from the C++, at its line, to the shader, or for
// a readback from the shader, at the member's line, to the C++.
void Relations::relate_seams() {
  for (const auto &[folder, in] : _in_folder)
    for (const Bound *bind : in.bound)
      for (const auto &[file, names] : in.declared) {
        const auto declared = std::ranges::find(*names, bind->name, &Declared::name);
        if (!bind->pass || declared == names->end())
          continue;
        const std::string cpp = bind->file.generic_string();
        const std::string glsl = file->generic_string();
        _found.push_back({.from = bind->sends ? cpp : glsl,
                          .to = bind->sends ? glsl : cpp,
                          .layer = VP_VIEW::Layer::engine,
                          .line = bind->sends ? bind->line : declared->line,
                          .said = bind->name});
      }
}

// A stage reads in what the nearest stage before it in its folder writes out, as its
// fragment shader reads its vertex shader's color, matched as the stages match them, by
// location, or by name where either gives none: the engine carries it from the writer,
// at the line writing it out, to the reader.
void Relations::relate_stages() {
  for (const auto &[reader, ins] : _varyings) {
    const std::size_t stage = *draw_stage(reader);
    const std::filesystem::path *writer = nullptr;
    const std::vector<Varying> *written = nullptr;
    for (const auto &[file, outs] : _in_folder.at(reader.parent_path()).varyings)
      if (*draw_stage(*file) < stage &&
          (!writer || *draw_stage(*file) > *draw_stage(*writer))) {
        writer = file;
        written = outs;
      }
    if (!writer)
      continue;
    for (const Varying &in : ins) {
      if (in.out)
        continue;
      const std::vector<Varying> &outs = *written;
      const auto out = std::ranges::find_if(outs, [&](const Varying &out) {
        return out.out && (in.location && out.location ? *in.location == *out.location
                                                       : in.name == out.name);
      });
      if (out != outs.end())
        _found.push_back({.from = writer->generic_string(),
                          .to = reader.generic_string(),
                          .layer = VP_VIEW::Layer::engine,
                          .line = out->line,
                          .said = in.name});
    }
  }
}

// A connection of a manifest in the folder shown, between two nodes in its folder, as
// RV08 places them: the engine carries it from the file of the writer's node that writes
// the port, at the line doing so, to the file of each reader's node that reads it, so a
// project's C++ and shaders show joined where its manifest joins them.
void Relations::relate_wires() {
  const auto folder_of = [](const Wired &wired, std::string_view end) {
    std::string node(end.substr(0, end.rfind('.')));
    std::ranges::replace(node, '.', '/');
    return wired.manifest.parent_path() / node;
  };
  const auto port_of = [](std::string_view end) {
    return end.substr(end.rfind('.') + 1);
  };
  for (const Wired &wired : _wired) {
    if (wired.from.find('.') == std::string::npos)
      continue; // a view's window, which no file of this folder writes
    const auto writer =
        port_file(folder_of(wired, wired.from), port_of(wired.from), true);
    for (const std::string &to : wired.to) {
      const auto reader = to.find('.') == std::string::npos
                              ? std::nullopt
                              : port_file(folder_of(wired, to), port_of(to), false);
      if (writer && reader && writer->first != reader->first)
        _found.push_back({.from = writer->first.generic_string(),
                          .to = reader->first.generic_string(),
                          .layer = VP_VIEW::Layer::engine,
                          .line = writer->second,
                          .said = wired.name});
    }
  }
}

// The file of a node's folder that writes a port, or reads it, and the line saying so:
// C++ binding it as data leaving it, or reaching it, else a shader whose pass block
// declares it, or whose stage writes it out, or reads it in; none for none.
std::optional<std::pair<std::filesystem::path, std::uint32_t>>
Relations::port_file(const std::filesystem::path &folder,
                     std::string_view port,
                     bool writes) const {
  const auto in = _in_folder.find(folder);
  if (in == _in_folder.end())
    return std::nullopt;
  for (const Bound *bind : in->second.bound)
    if (bind->name == port && bind->sends == writes)
      return std::pair(bind->file, bind->line);
  for (const auto &[file, names] : in->second.declared)
    if (const auto declared = std::ranges::find(*names, port, &Declared::name);
        declared != names->end())
      return std::pair(*file, declared->line);
  for (const auto &[file, varyings] : in->second.varyings)
    if (const auto varying = std::ranges::find_if(*varyings,
                                                  [&](const Varying &varying) {
                                                    return varying.name == port &&
                                                           varying.out == writes;
                                                  });
        varying != varyings->end())
      return std::pair(*file, varying->line);
  return std::nullopt;
}

VP_OPERATOR(Relations);
