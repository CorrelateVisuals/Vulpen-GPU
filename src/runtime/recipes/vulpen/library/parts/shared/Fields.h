#pragma once

#include "baseclasses/Shader.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Ini.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// What a shader's pass block may hold, read alike by the line that declares a field, the
// terminal's Tab and the menus' column of types (RV09), so the three offer what the
// engine takes.

// The values a param sets, the commonest first, by the names the engine reads them by.
constexpr std::array value_types{VP::glsl_type<float>,
                                 VP::glsl_type<glm::vec2>,
                                 VP::glsl_type<glm::vec3>,
                                 VP::glsl_type<glm::vec4>,
                                 VP::glsl_type<std::int32_t>,
                                 VP::glsl_type<std::uint32_t>,
                                 VP::glsl_type<glm::ivec2>,
                                 VP::glsl_type<glm::ivec3>,
                                 VP::glsl_type<glm::ivec4>,
                                 VP::glsl_type<glm::uvec2>,
                                 VP::glsl_type<glm::uvec3>,
                                 VP::glsl_type<glm::uvec4>};

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

// The rows of a text, without their line breaks, the last after the last break.
inline std::vector<std::string> rows_of_text(std::string_view text) {
  std::vector<std::string> rows;
  for (std::size_t at = 0; at <= text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    rows.emplace_back(text.substr(at, end - at));
    at = end + 1;
  }
  return rows;
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

// The types a shader may declare a pass block field of: the values, a Texture to sample,
// and the buffers it reads, and in a .comp an Image to store into and the buffers it
// writes, as a draw's shaders only read (RVK01); none for a file that is no shader or is
// not there, or a lone .frag.
inline std::vector<std::string> field_types(VP::FilePort &files,
                                            const std::filesystem::path &file) {
  const std::string extension = file.extension().string();
  if (std::ranges::find(VP::stages, extension) == VP::stages.end() ||
      !listed_file(files, file) || lone_fragment(files.list(file.parent_path().string())))
    return {};
  const bool dispatch = extension == VP::compute_stage;
  std::vector<std::string> types(value_types.begin(), value_types.end());
  types.emplace_back(VP::texture_type);
  if (dispatch)
    types.emplace_back(VP::image_type);
  for (const BufferType &buffer : buffers_of(files, file, files.read(file.string())))
    if (dispatch || !buffer.writes)
      types.push_back(buffer.name);
  return types;
}

} // namespace
