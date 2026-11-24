#pragma once

// What follows a rename, kept apart from the files part's commands, so neither file grows
// past what one reads whole (CPP12): the includes of a header or a GLSL file renamed, the
// class of a C++ file renamed, and the graph's edits of a node renamed. First, two
// helpers both files use.

#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// "Ye call me Master and Lord: and ye say well; for so I am." John 13:13

// Without the / that may end a folder's path, so its name is its last part.
std::filesystem::path bare(const std::filesystem::path &path) {
  const std::filesystem::path normal = path.lexically_normal();
  return normal.filename().empty() ? normal.parent_path() : normal;
}

bool word_letter(char letter) {
  return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
         (letter >= '0' && letter <= '9') || letter == '_';
}

// The sources below a folder, at any depth but the hidden folders', that may include a
// header or a GLSL file: C++'s and the shaders'.
void sources_below(VP::FilePort &files,
                   const std::filesystem::path &folder,
                   std::vector<std::filesystem::path> &found) {
  constexpr std::array<std::string_view, 10> sources{".cpp",
                                                     ".h",
                                                     ".hpp",
                                                     ".comp",
                                                     ".vert",
                                                     ".tesc",
                                                     ".tese",
                                                     ".geom",
                                                     ".frag",
                                                     ".glsl"};
  for (const std::string &name : files.list(folder.string()))
    if (name.starts_with('.'))
      continue;
    else if (name.ends_with('/'))
      sources_below(files, folder / bare(name), found);
    else if (std::ranges::find(sources,
                               std::filesystem::path(name).extension().string()) !=
             sources.end())
      found.push_back(folder / name);
}

// A header or a GLSL file renamed in a view keeps its includers building, as a contract
// or shared code is included by many (RV05, RV09): each #include "..." of the view's
// sources that named it, from the including file's folder or from the view's top, names
// the new path the same way. How many sources changed.
std::size_t follow_includes(VP::FilePort &files,
                            const std::filesystem::path &top,
                            const std::filesystem::path &from,
                            const std::filesystem::path &to) {
  constexpr std::string_view include = "#include \"";
  std::vector<std::filesystem::path> sources;
  sources_below(files, top, sources);
  std::size_t changed = 0;
  for (const std::filesystem::path &source : sources) {
    std::string text = files.read(source.string());
    bool named = false;
    for (std::size_t at = text.find(include); at != std::string::npos;
         at = text.find(include, at + 1)) {
      const std::size_t open = at + include.size();
      const std::size_t close = text.find('"', open);
      if (close == std::string::npos)
        break;
      const std::filesystem::path written(text.substr(open, close - open));
      for (const std::filesystem::path &base : {source.parent_path(), top})
        if ((base / written).lexically_normal() == from) {
          text.replace(open, close - open, to.lexically_relative(base).generic_string());
          named = true;
          break;
        }
    }
    if (named) {
      files.save(source.string(), text);
      ++changed;
    }
  }
  return changed;
}

// A C++ file named for its class, as new starts one, Clock.cpp holding class Clock,
// renamed renames the class in it, each whole word, its registration too, so the file
// still names its class; whether it did. Not a header: its includers name its type too.
bool follow_class(VP::FilePort &files,
                  const std::filesystem::path &from,
                  const std::filesystem::path &to) {
  const std::string old_name = from.stem().string();
  const std::string new_name = to.stem().string();
  const auto identifier = [](std::string_view name) {
    return !name.empty() && !(name.front() >= '0' && name.front() <= '9') &&
           std::ranges::all_of(name, word_letter);
  };
  if (to.extension() != ".cpp" || old_name == new_name || !identifier(old_name) ||
      !identifier(new_name))
    return false;
  std::string text = files.read(to.string());
  const auto whole = [&](std::size_t at, std::size_t size) {
    return (at == 0 || !word_letter(text[at - 1])) &&
           (at + size == text.size() || !word_letter(text[at + size]));
  };
  const auto declares = [&](std::string_view kind) {
    const std::string declared = std::format("{} {}", kind, old_name);
    for (std::size_t at = text.find(declared); at != std::string::npos;
         at = text.find(declared, at + 1))
      if (whole(at, declared.size()))
        return true;
    return false;
  };
  if (!declares("class") && !declares("struct"))
    return false;
  for (std::size_t at = text.find(old_name); at != std::string::npos;
       at = text.find(old_name, at))
    if (whole(at, old_name.size())) {
      text.replace(at, old_name.size(), new_name);
      at += new_name.size();
    } else {
      at += old_name.size();
    }
  files.save(to.string(), text);
  return true;
}

// A command's words carry no blanks, so a value holding one cannot be sent.
std::string sendable(std::string_view key, std::string_view value) {
  if (value.find_first_of(" \t") != std::string_view::npos)
    throw std::runtime_error(std::format(
        "{} {} holds a blank, which a command's word cannot carry", key, value));
  return std::format(" {}={}", key, value);
}

// The graph's side of a node's rename, as the edits a person would type, so a replay
// renames it alike (V08): the connections reaching the node or the nodes inside it out,
// the node out, and each of them back under the new name with the words it had, in the
// order they were, back where the node was among the others, and the connections back,
// those named inside it renamed too.
std::vector<std::string>
renames(const VP::View &view, const std::string &from, const std::string &to) {
  const auto moved = [&](std::string_view name) -> std::optional<std::string> {
    if (!VP::within(name, from))
      return std::nullopt;
    return std::format("{}{}", to, name.substr(from.size()));
  };
  const auto end = [&](VP::Endpoint endpoint) {
    if (!endpoint.view())
      endpoint.node = moved(endpoint.node).value_or(endpoint.node);
    return endpoint.text();
  };
  std::vector<std::string> lines;
  std::vector<const VP::Connection *> reaching;
  for (const VP::Connection &connection : view.connections) {
    const auto inside = [&](const VP::Endpoint &endpoint) {
      return !endpoint.view() && moved(endpoint.node);
    };
    if (inside(connection.from) || std::ranges::any_of(connection.to, inside)) {
      reaching.push_back(&connection);
      lines.push_back(std::format("disconnect {}", connection.name));
    }
  }
  lines.push_back(std::format("node remove keep {}", from));
  std::optional<std::string> after;
  bool past = false;
  for (const VP::Node &node : view.nodes) {
    if (const std::optional<std::string> name = moved(node.name)) {
      past = true;
      std::string line = std::format("node add {}", *name);
      for (const VP::Word &word : VP::words_of(node))
        line += sendable(word.key, word.value);
      lines.push_back(std::move(line));
    } else if (past && !after) {
      after = node.name;
    }
  }
  if (after)
    lines.push_back(std::format("node move {} {}", to, *after));
  // Only a name inside the node's is the node's, as a drop names a connection inside the
  // recipe; one named as the node is, as corners carrying corners, keeps its name.
  for (const VP::Connection *const connection : reaching) {
    const std::string &name = connection->name;
    std::string line = std::format("connect {} {}",
                                   name == from ? name : moved(name).value_or(name),
                                   end(connection->from));
    for (const VP::Endpoint &reader : connection->to)
      line += ' ' + end(reader);
    lines.push_back(std::move(line));
  }
  return lines;
}

} // namespace
