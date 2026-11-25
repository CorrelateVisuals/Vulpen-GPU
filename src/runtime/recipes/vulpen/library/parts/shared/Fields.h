#pragma once

#include "baseclasses/Shader.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Ini.h"
#include "shared/Words.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// What a shader's pass block may hold, and what a node's C++ may declare, read alike by
// the line that declares one, the terminal's Tab and the menus' columns (RV09), so the
// three offer what the engine takes.

// "And ye are complete in him, which is the head of all principality and power:"
// Colossians 2:10

// A value a param sets: its type as GLSL names it, which the engine reads it by, and as
// C++ spells the type a node's C++ reads it into.
struct ValueType {
  std::string_view glsl;
  std::string_view cpp;
};

template <class T> constexpr ValueType value_type(std::string_view cpp) {
  return {VP::glsl_type<T>, cpp};
}

// The commonest first.
constexpr std::array value_types{value_type<float>("float"),
                                 value_type<glm::vec2>("glm::vec2"),
                                 value_type<glm::vec3>("glm::vec3"),
                                 value_type<glm::vec4>("glm::vec4"),
                                 value_type<std::int32_t>("std::int32_t"),
                                 value_type<std::uint32_t>("std::uint32_t"),
                                 value_type<glm::ivec2>("glm::ivec2"),
                                 value_type<glm::ivec3>("glm::ivec3"),
                                 value_type<glm::ivec4>("glm::ivec4"),
                                 value_type<glm::uvec2>("glm::uvec2"),
                                 value_type<glm::uvec3>("glm::uvec3"),
                                 value_type<glm::uvec4>("glm::uvec4")};
// And a word, as a file's name, which only C++ reads, as Bind::param takes it.
constexpr ValueType word_type{"string", "std::string"};

// The C++ of a node's folder, which builds as its module (RV08): its .cpp files and the
// headers beside them.
constexpr std::string_view cpp_extension = ".cpp";
constexpr std::string_view header_extension = ".h";

// A buffer type, as a row VP_READS(Corners, Corner); declares one: its name, its element,
// whether its shader reads it, writes it, or both, as VP_BUFFER declares, and whether the
// row is commented out, as a start's hint, for a person to uncomment.
struct BufferType {
  std::string name;
  std::string element;
  bool reads = true;
  bool writes = false;
  bool commented = false;
};

// The macros that declare one, as baseclasses/GpuLayout.glsl defines them, reading,
// writing and both, and the buffer types it declares itself, which every shader that
// includes it holds.
constexpr std::array<std::string_view, 3> buffer_macros{
    "VP_READS(", "VP_WRITES(", "VP_BUFFER("};
constexpr std::string_view gpu_layout = "baseclasses/GpuLayout.glsl";
inline std::vector<BufferType> layout_buffers() {
  return {{"FloatsIn", "float", true, false, false},
          {"FloatsOut", "float", false, true, false},
          {"Floats", "float", true, true, false}};
}

// A row's code with the // that comments it out taken off, and the blank after it, so a
// start's hint reads as the row it was; none for a row no // starts.
inline std::optional<std::string> uncommented(std::string_view row) {
  if (!trim(row).starts_with("//"))
    return std::nullopt;
  const std::size_t at = row.find("//");
  const std::size_t past = at + 2 + (row.substr(at + 2).starts_with(' ') ? 1 : 0);
  return std::string(row.substr(0, at)).append(row.substr(past));
}

// The buffer type a row declares, or with commented, one a commented-out row would.
inline std::optional<BufferType> buffer_declared(std::string_view row, bool commented) {
  const std::optional<std::string> hint = commented ? uncommented(row) : std::nullopt;
  if (commented && !hint)
    return std::nullopt;
  const std::string_view code = trim(hint ? std::string_view(*hint) : row);
  for (std::size_t kind = 0; kind < buffer_macros.size(); ++kind) {
    const std::size_t open = buffer_macros[kind].size();
    const std::size_t comma = code.find(',');
    const std::size_t close = code.find(')');
    if (!code.starts_with(buffer_macros[kind]) || close == std::string_view::npos ||
        comma > close)
      continue;
    return BufferType{.name = std::string(trim(code.substr(open, comma - open))),
                      .element =
                          std::string(trim(code.substr(comma + 1, close - comma - 1))),
                      .reads = kind != 1,
                      .writes = kind != 0,
                      .commented = commented};
  }
  return std::nullopt;
}

// What a live #include "..." row names; none for any other row.
inline std::optional<std::string> include_of(std::string_view row) {
  constexpr std::string_view include = "#include \"";
  const std::string_view code = trim(row);
  const std::size_t close = code.find('"', include.size());
  if (!code.starts_with(include) || close == std::string_view::npos)
    return std::nullopt;
  return std::string(code.substr(include.size(), close - include.size()));
}

// Whether a file is there, by what its folder lists.
inline bool listed_file(VP::FilePort &files, const std::filesystem::path &path) {
  const std::vector<std::string> names = files.list(path.parent_path().string());
  return std::ranges::find(names, path.filename().string()) != names.end();
}

// Where an include resolves for the build: beside the file, else in the nearest folder
// above it that holds it, as a view's top holds its contracts/ and the library's its own;
// none for one found nowhere, as GpuLayout.glsl from a project, whose types are known.
inline std::optional<std::filesystem::path> resolved_include(
    VP::FilePort &files, const std::filesystem::path &file, std::string_view include) {
  for (std::filesystem::path at = file.parent_path();; at = at.parent_path()) {
    if (const std::filesystem::path path = (at / include).lexically_normal();
        listed_file(files, path))
      return path;
    if (at == at.parent_path())
      return std::nullopt;
  }
}

// The buffer types a shader holds: GpuLayout's, those it declares, live or commented out
// for a person to uncomment, and those the files it includes declare.
inline std::vector<BufferType> buffers_of(VP::FilePort &files,
                                          const std::filesystem::path &file,
                                          std::string_view text) {
  std::vector<BufferType> found = layout_buffers();
  const auto add = [&](std::optional<BufferType> type) {
    if (type && std::ranges::find(found, type->name, &BufferType::name) == found.end())
      found.push_back(std::move(*type));
  };
  for (const std::string &row : rows_of_text(text)) {
    add(buffer_declared(row, false));
    add(buffer_declared(row, true));
    const std::optional<std::string> include = include_of(row);
    if (!include || *include == gpu_layout)
      continue;
    if (const std::optional<std::filesystem::path> path =
            resolved_include(files, file, *include))
      for (const std::string &included : rows_of_text(files.read(path->string())))
        add(buffer_declared(included, false));
  }
  return found;
}

// Whether a folder's draw stages are a .frag alone: a fragment node's, which reads the
// pass block of the .vert it draws with and declares none.
inline bool lone_fragment(const std::vector<std::string> &names) {
  const auto holds = [&](std::string_view stage) {
    return std::ranges::any_of(
        names, [&](const std::string &name) { return name.ends_with(stage); });
  };
  return std::ranges::all_of(VP::stages, [&](std::string_view stage) {
    return stage == VP::compute_stage || holds(stage) == (stage == VP::fragment_stage);
  });
}

// A pass block's field, as its row declares it: `type name;`.
struct Declared {
  std::string type;
  std::string name;

  bool operator==(const Declared &) const = default;
};

// A name GLSL takes for a field, which a port's is too: a letter or _ first, then
// letters, digits and _, and no gl_ first, which GLSL keeps for its own.
inline bool glsl_name(std::string_view name) {
  const auto letter = [](char each) {
    return (each >= 'a' && each <= 'z') || (each >= 'A' && each <= 'Z') || each == '_';
  };
  return !name.empty() && letter(name.front()) && !name.starts_with("gl_") &&
         std::ranges::all_of(name, [&](char each) {
           return letter(each) || (each >= '0' && each <= '9');
         });
}

// The field a row's code declares, a comment after it or not; none for any other row.
inline std::optional<Declared> field_of(std::string_view code) {
  code = trim(code.substr(0, code.find("//")));
  if (!code.ends_with(';'))
    return std::nullopt;
  code = trim(code.substr(0, code.size() - 1));
  const std::size_t blank = code.find_first_of(" \t");
  if (blank == std::string_view::npos)
    return std::nullopt;
  Declared field{std::string(code.substr(0, blank)),
                 std::string(trim(code.substr(blank)))};
  if (!glsl_name(field.type) || !glsl_name(field.name))
    return std::nullopt;
  return field;
}

inline bool opens_block(std::string_view code) {
  code = trim(code);
  return code.starts_with("layout(") &&
         code.find("uniform Pass") != std::string_view::npos;
}

// The fields a shader's live pass block declares, in their order; none for a text whose
// block is commented out, or not closed. A field may share its row with the block's
// braces, as in a block written on one row, so a row is read from its { to its }.
inline std::vector<Declared> live_fields(std::string_view text) {
  std::vector<Declared> fields;
  bool inside = false;
  for (const std::string &row : rows_of_text(text)) {
    const std::string_view line = row;
    std::string_view code = trim(line.substr(0, line.find("//")));
    if (!inside && !(inside = opens_block(code)))
      continue;
    if (const std::size_t open = code.find('{'); open != std::string_view::npos)
      code.remove_prefix(open + 1);
    const std::size_t close = code.find('}');
    for (const auto declared : std::views::split(code.substr(0, close), ';'))
      if (std::optional<Declared> field =
              field_of(std::string(declared.begin(), declared.end()) + ';'))
        fields.push_back(std::move(*field));
    if (close != std::string_view::npos)
      return fields;
  }
  return {};
}

// A node's shaders, by the folder holding them (RV08), as they share one pass block: the
// first that declares a live one, or includes a file beside it that does, as a part's
// shaders share theirs so, and its fields; none for a folder with none, as a lone
// .frag's, which reads its .vert's.
struct NodeFields {
  std::filesystem::path shader; // whose buffer types type the fields
  std::vector<Declared> fields;
};

inline NodeFields node_fields(VP::FilePort &files, const std::filesystem::path &folder) {
  for (const std::string &name : files.list(folder.string())) {
    const std::filesystem::path shader = folder / name;
    if (std::ranges::find(VP::stages, shader.extension().string()) == VP::stages.end())
      continue;
    const std::string text = files.read(shader.string());
    if (std::vector<Declared> fields = live_fields(text); !fields.empty())
      return {shader, std::move(fields)};
    for (const std::string &row : rows_of_text(text))
      if (const std::optional<std::string> include = include_of(row))
        if (const std::filesystem::path beside = (folder / *include).lexically_normal();
            listed_file(files, beside))
          if (std::vector<Declared> fields = live_fields(files.read(beside.string()));
              !fields.empty())
            return {shader, std::move(fields)};
  }
  return {};
}

// The node of a view whose folder holds a file (RV08), root at its top; none for a file
// no node of the view holds.
inline std::optional<std::string> node_named(const VP::View &view,
                                             const std::filesystem::path &file) {
  const std::filesystem::path inside = file.parent_path().lexically_relative(
      std::filesystem::absolute(view.file.parent_path()).lexically_normal());
  std::string name;
  for (const std::filesystem::path &part : inside)
    if (part != ".")
      name.append(name.empty() ? "" : ".").append(part.string());
  if (name.empty())
    name = VP::top_node;
  if (inside.empty() || *inside.begin() == ".." ||
      std::ranges::find(view.nodes, name, &VP::Node::name) == view.nodes.end())
    return std::nullopt;
  return name;
}

// A field of its node's shaders as a line declaring it in a .cpp finds it, and why the
// line may not, where it may not: an Image, which the dispatch stores into; a value a
// param of the node sets; one a connection fills; or one the node's C++ names already, as
// a line declared it before. Empty where it may.
struct CppField {
  Declared field;
  std::string taken;
};

// Each field of the shaders of a .cpp's node, which its C++ may fill or read back by a
// line, as nothing does yet; none for a .cpp of no node of the view.
inline std::vector<CppField>
cpp_fields(VP::FilePort &files, const std::filesystem::path &cpp, const VP::View &view) {
  const std::optional<std::string> node = node_named(view, cpp);
  if (!node || cpp.extension() != cpp_extension)
    return {};
  const std::filesystem::path folder = cpp.parent_path();
  std::vector<std::pair<std::string, std::string>> code; // C++ rows by their file
  for (const std::string &name : files.list(folder.string()))
    if (name.ends_with(cpp_extension) || name.ends_with(header_extension))
      for (std::string &row : rows_of_text(files.read((folder / name).string())))
        if (!trim(row).starts_with("//"))
          code.emplace_back(name, std::move(row));
  const auto own = std::ranges::find(view.nodes, *node, &VP::Node::name);
  std::vector<CppField> found;
  for (Declared &field : node_fields(files, folder).fields) {
    std::string taken;
    const std::string called = std::format("(\"{}\"", field.name);
    const auto named = std::ranges::find_if(code, [&](const auto &row) {
      return row.second.find(called) != std::string::npos;
    });
    if (field.type == VP::image_type)
      taken = std::format("{} is an Image, which its dispatch stores into", field.name);
    else if (own != view.nodes.end() && own->param(field.name))
      taken = std::format("param {} of {} sets it", field.name, *node);
    else if (named != code.end())
      taken = std::format("{} names it already", named->first);
    for (const VP::Connection &connection : view.connections)
      for (const VP::Endpoint &to : connection.to)
        if (taken.empty() && to.node == *node && to.port == field.name)
          taken = std::format("connection {} fills it", connection.name);
    found.push_back({std::move(field), std::move(taken)});
  }
  return found;
}

// The names of those its C++ may take, as Tab and the menus offer them.
inline std::vector<std::string>
free_fields(VP::FilePort &files, const std::filesystem::path &cpp, const VP::View &view) {
  std::vector<std::string> names;
  for (const CppField &each : cpp_fields(files, cpp, view))
    if (each.taken.empty())
      names.push_back(each.field.name);
  return names;
}

// A type a view's C++ contract declares (RV05), as `struct Spin {` or `using Rects =` at
// the top of namespace VP_VIEW, by its name, and the header declaring it, as an include
// names it, contracts/Spin.h.
struct CppContract {
  std::string type;
  std::string header;
};

// The type a contract's row declares at the top of its namespace, where the row starts
// with it, as clang-format writes it: a struct, aligned or not, or a name for a type;
// none for any other row, as a struct only declared, or one commented out.
inline std::optional<std::string> contract_type(std::string_view row) {
  const bool alias = row.starts_with("using ");
  if (!alias && !row.starts_with("struct "))
    return std::nullopt;
  std::string_view code = trim(row.substr(row.find(' ')));
  if (!alias && code.starts_with("alignas(") && code.find(')') != std::string_view::npos)
    code = trim(code.substr(code.find(')') + 1));
  const std::size_t end = code.find_first_of(" {=:;");
  if (end == std::string_view::npos || !glsl_name(code.substr(0, end)))
    return std::nullopt;
  const std::string_view rest = trim(code.substr(end));
  const bool defines =
      alias ? rest.starts_with('=')
            : rest.starts_with('{') || rest.starts_with(':') || rest.starts_with("final");
  return defines ? std::optional(std::string(code.substr(0, end))) : std::nullopt;
}

// The C++ contracts a node's code may include, as its build resolves contracts/ from the
// top of its view, or of the library: those of the headers in the contracts/ of the
// nearest folder above the file that holds one.
inline std::vector<CppContract> cpp_contracts(VP::FilePort &files,
                                              const std::filesystem::path &file) {
  for (std::filesystem::path at = file.parent_path();; at = at.parent_path()) {
    const std::filesystem::path folder = at / VP::contracts_folder;
    if (const std::vector<std::string> names = files.list(folder.string());
        !names.empty()) {
      std::vector<CppContract> found;
      for (const std::string &name : names) {
        if (!name.ends_with(header_extension))
          continue;
        for (const std::string &row : rows_of_text(files.read((folder / name).string())))
          if (std::optional<std::string> type = contract_type(row))
            found.push_back(
                {std::move(*type), std::format("{}/{}", VP::contracts_folder, name)});
      }
      return found;
    }
    if (at == at.parent_path())
      return {};
  }
}

// The types a shader may declare a pass block field of: the values, a Texture to sample,
// and the buffers it reads, and in a .comp an Image to store into and the buffers it
// writes, as a draw's shaders only read (RVK01); a .cpp a param of, a value or a word, or
// an output of, a type its view's contracts declare. None for a file that is neither or
// is not there, or a lone .frag.
inline std::vector<std::string> field_types(VP::FilePort &files,
                                            const std::filesystem::path &file) {
  const std::string extension = file.extension().string();
  std::vector<std::string> types;
  for (const ValueType &value : value_types)
    types.emplace_back(value.glsl);
  if (extension == cpp_extension && listed_file(files, file)) {
    types.emplace_back(word_type.glsl);
    for (CppContract &contract : cpp_contracts(files, file))
      types.push_back(std::move(contract.type));
    return types;
  }
  if (std::ranges::find(VP::stages, extension) == VP::stages.end() ||
      !listed_file(files, file) || lone_fragment(files.list(file.parent_path().string())))
    return {};
  const bool dispatch = extension == VP::compute_stage;
  types.emplace_back(VP::texture_type);
  if (dispatch)
    types.emplace_back(VP::image_type);
  for (const BufferType &buffer : buffers_of(files, file, files.read(file.string())))
    if (dispatch || !buffer.writes)
      types.push_back(buffer.name);
  return types;
}

} // namespace
