#include "contracts/Mesh.h"
#include "runtime/Operator.h"

// tinyobj has no switch that keeps its code to the file that includes it, as stb's
// STATIC does, so copies of this part in two views would clash in one release binary: it
// goes inside the unnamed namespace, each copy its own, as RV09 keeps shared code. The
// standard headers it includes come first, so none of them lands in there.
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
namespace {
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
} // namespace

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

// "Wherefore when he cometh into the world, he saith, Sacrifice and offering thou
// wouldest not, but a body hast thou prepared me:" Hebrews 10:5

constexpr std::size_t corners = 3; // a triangle's, as tinyobj cuts every face into them
constexpr std::size_t components = 3; // a position's, a normal's and a color's, in a row

// The three numbers of an attribute at index, as tinyobj lists them, a row each; throws
// when the file's face names one past the list, which tinyobj leaves to its reader.
glm::vec3
row(const std::vector<tinyobj::real_t> &list, int index, std::string_view what) {
  const std::size_t rows = list.size() / components;
  if (index < 0 || static_cast<std::size_t>(index) >= rows)
    throw std::runtime_error(std::format(
        "a face names {} {} of the {} the file lists", what, index + 1, rows));
  const std::size_t at = static_cast<std::size_t>(index) * components;
  return {list[at], list[at + 1], list[at + 2]};
}

// A face cut to a triangle, its normals the file's, or its own where the file gives none.
VP_VIEW::Triangle triangle(const tinyobj::attrib_t &attrib,
                           std::span<const tinyobj::index_t> face) {
  std::array<glm::vec3, corners> at{}, normal{};
  glm::vec3 color{0.0f};
  for (std::size_t corner = 0; corner < corners; ++corner) {
    at[corner] = row(attrib.vertices, face[corner].vertex_index, "vertex");
    color += attrib.colors.empty()
                 ? glm::vec3(1.0f)
                 : row(attrib.colors, face[corner].vertex_index, "vertex color");
  }
  const glm::vec3 across = glm::cross(at[1] - at[0], at[2] - at[0]);
  const float area = glm::length(across);
  for (std::size_t corner = 0; corner < corners; ++corner)
    normal[corner] = face[corner].normal_index >= 0
                         ? row(attrib.normals, face[corner].normal_index, "normal")
                     : area > 0.0f ? across / area
                                   : glm::vec3(0.0f);
  return {.a = glm::vec4(at[0], 1.0f),
          .b = glm::vec4(at[1], 1.0f),
          .c = glm::vec4(at[2], 1.0f),
          .normal_a = glm::vec4(normal[0], 0.0f),
          .normal_b = glm::vec4(normal[1], 0.0f),
          .normal_c = glm::vec4(normal[2], 0.0f),
          .color = glm::vec4(color / static_cast<float>(corners), 1.0f)};
}

// Every face of every object in the OBJ text, cut into triangles. Throws with tinyobj's
// reason, or the face that names what the file lacks.
std::vector<VP_VIEW::Triangle> triangles_of(std::string_view text) {
  tinyobj::ObjReader reader;
  if (!reader.ParseFromString(std::string(text), ""))
    throw std::runtime_error(std::format("no OBJ tinyobj reads: {}", reader.Error()));
  std::vector<VP_VIEW::Triangle> triangles;
  for (const tinyobj::shape_t &shape : reader.GetShapes()) {
    const std::span<const tinyobj::index_t> indices = shape.mesh.indices;
    for (std::size_t first = 0; first + corners <= indices.size(); first += corners)
      triangles.push_back(triangle(reader.GetAttrib(), indices.subspan(first, corners)));
  }
  return triangles;
}

// The OBJ file its file param names, its faces cut into triangles, written at triangles
// for the node a connection takes them to: read again once it changes on disk, so a
// model saved elsewhere shows at once. Materials and textures stay out: the mesh part
// tints. A file it cannot parse, or of more triangles than most holds, is named in the
// log, and the triangles keep what they held.
class MeshFile final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _file = node.sets("file") ? node.param<std::string>("file") : "";
    _most = node.param<std::uint32_t>("most");
    _name = node.name();
    _view = node.view_name();
    _triangles = node.upload<VP_VIEW::Triangle>("triangles", _most);
    _watched = _file.empty() ? VP::File{} : node.file(_file);
    _written = false; // a rebuild may make the buffer anew
  }

  void cook(VP::Cook &frame) override {
    if (_file.empty())
      return;
    const std::string_view text = frame.files().text(_watched);
    const std::size_t read = std::hash<std::string_view>{}(text);
    if (_parsed != read) {
      _parsed = read; // parsed once, whether it parses or not, until it changes
      try {
        _mesh = triangles_of(text);
        _written = false;
      } catch (const std::runtime_error &failure) {
        frame.log(VP::Level::error, std::format("{}: {}", _file, failure.what()));
      }
    }
    if (std::exchange(_written, true))
      return;
    if (_mesh.size() > _most) {
      frame.log(VP::Level::error,
                std::format("{} holds {} triangles, more than most = {}: {}param set {} "
                            "most {} makes room",
                            _file,
                            _mesh.size(),
                            _most,
                            _view.empty() ? "" : _view + ": ",
                            _name,
                            _mesh.size()));
      return;
    }
    std::ranges::copy(_mesh, frame.write(_triangles, _mesh.size()).begin());
    frame.log(VP::Level::info, std::format("{}: {} triangles", _file, _mesh.size()));
  }

  std::string _file; // the param's, as an absolute path; empty for none
  std::uint32_t _most = 0;
  std::string _name;
  std::string _view; // the one it runs in, as a line names it; empty for vulpen's own
  VP::Upload<VP_VIEW::Triangle> _triangles;
  VP::File _watched;
  // The file's text the triangles hold, by its hash; none until one parsed.
  std::optional<std::size_t> _parsed;
  std::vector<VP_VIEW::Triangle> _mesh; // as last parsed, written again after a rebuild
  bool _written = false;
};

} // namespace

VP_OPERATOR(MeshFile);
