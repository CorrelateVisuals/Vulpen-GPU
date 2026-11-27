#include "mesh-file/MeshFile.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/mesh-file: the OBJ file its file param names, its faces cut into
// triangles, each corner's normal the file's or the face's own, its color the corners'
// average; written once a version, and again after a bind; a file it cannot read, or of
// more triangles than most holds, named in the log, the triangles keeping what they held.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// A triangle; a square of two, its normal given; a triangle of a color a corner.
constexpr std::string_view one = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view square =
    "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 0 2 0\nvn 0 0 -1\nf 1//1 2//1 3//1 4//1\n";
constexpr std::string_view colored =
    "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nf 1 2 3\n";
// A face naming a vertex the file does not list.
constexpr std::string_view broken = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n";

// A vector's x, y and z: a triangle's corners, normals and color have no w to read.
std::string xyz(const glm::vec4 &vector) {
  return std::format(
      "({} {} {})", number_text(vector.x), number_text(vector.y), number_text(vector.z));
}

std::string triangle_text(const VP_VIEW::Triangle &triangle) {
  return std::format("{}{}{} normals {}{}{} color {}",
                     xyz(triangle.a),
                     xyz(triangle.b),
                     xyz(triangle.c),
                     xyz(triangle.normal_a),
                     xyz(triangle.normal_b),
                     xyz(triangle.normal_c),
                     xyz(triangle.color));
}

std::string triangles_text(const std::vector<VP_VIEW::Triangle> &triangles) {
  std::string text = std::format("{} triangles", triangles.size());
  for (const VP_VIEW::Triangle &triangle : triangles)
    text += "; " + triangle_text(triangle);
  return text;
}

// No count written this cook.
constexpr std::size_t unwritten = std::numeric_limits<std::size_t>::max();

// The node in its view v as n, in a folder holding one.obj, square.obj and broken.obj.
struct Framed {
  Running<MeshFile> node;
  std::filesystem::path folder;

  Framed(const Rows &rows, std::string_view file, std::string_view most = "16")
      : folder(planted(rows,
                       {"one.obj=" + std::string(one),
                        "square.obj=" + std::string(square),
                        "broken.obj=" + std::string(broken)})) {
    node.bind.view_named = "v";
    bound(file, most);
    node.cook.file_port = &node.files;
  }
  // Bound with this file in its folder, none for empty, as a param set or a rebuild
  // binds it.
  void bound(std::string_view file, std::string_view most = "16") {
    node.bind.params = {{"most", std::string(most)}};
    if (!file.empty())
      node.bind.params.emplace_back("file", (folder / file).string());
    node.bound();
  }
  // A cook: what it wrote, if anything, and what it logged. Each bind asks for its
  // upload anew, so the room it writes is the last.
  std::string cooked(Rows &rows) {
    node.cook.said.clear();
    node.cook.used.back() = unwritten;
    node.cooked();
    const std::size_t used = node.cook.used.back();
    std::string wrote = "wrote none";
    if (used != unwritten) {
      const auto *const triangles =
          reinterpret_cast<const VP_VIEW::Triangle *>(node.cook.written.back().data());
      wrote = "wrote " + triangles_text({triangles, triangles + used});
    }
    return here(std::format("{}; logged {}", wrote, show(node.cook.said, rows.names())),
                folder);
  }
};

} // namespace

void cases(Bench &bench) {
  bench.check("MeshFile.cpp/MeshFile::bind", [](Rows &rows) {
    for (const Params &params :
         {Params{{"file", "/p/a.obj"}, {"most", "4"}}, Params{{"most", "65536"}}}) {
      Running<MeshFile> node(params);
      node.bound();
      rows.row(std::format(
          "{} -> {}", show(params, rows.names()), show(node.bind.asked, rows.names())));
    }
  });

  // OBJ texts cut into triangles: normals the file's, or the face's own, none for a face
  // of no area; colors the corners' average; a face naming what the file lacks refused.
  bench.check("MeshFile.cpp/triangles_of", [](Rows &rows) {
    const std::vector<std::pair<std::string_view, std::string>> texts{
        {"a triangle", std::string(one)},
        {"a square, its normal given", std::string(square)},
        {"a color a corner", std::string(colored)},
        {"a color on one corner only", "v 0 0 0 1 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"},
        {"a face of no area", "v 1 1 1\nv 1 1 1\nv 1 1 1\nf 1 2 3\n"},
        {"a face naming vertex 9 of 3", std::string(broken)},
        {"a face naming normal 2 of 1",
         "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//2 2//2 3//2\n"},
        {"a face naming vertex 0", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n"},
        {"a face of two corners", "v 0 0 0\nv 1 0 0\nf 1 2\n"},
        {"lines and points only", "v 0 0 0\nv 1 0 0\nl 1 2\np 1\n"},
        {"no OBJ", "hello, world\n"},
        {"empty", ""}};
    for (const auto &[what, text] : texts) {
      std::string said;
      try {
        said = triangles_text(triangles_of(text));
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", what, said));
    }
  });

  // An attribute's three numbers at an index; one past the list refused, naming it as the
  // file counts, from 1.
  bench.check("MeshFile.cpp/row", [](Rows &rows) {
    const std::vector<tinyobj::real_t> list{0, 1, 2, 3, 4, 5, 6};
    for (const int index : {0, 1, 2, -1}) {
      std::string said;
      try {
        said = xyz(glm::vec4(row(list, index, "vertex"), 0.0f));
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", index, said));
    }
  });

  // Written once a version, and again after a bind, as a rebuild may make the buffer
  // anew; a file that does not parse named in the log, the triangles keeping what they
  // held; more than most holds named with the line that makes room; no file, nothing.
  bench.check("MeshFile.cpp/MeshFile::cook", [](Rows &rows) {
    Framed frame(rows, "one.obj");
    rows.row("one.obj named -> " + frame.cooked(rows));
    rows.row("its version as before -> " + frame.cooked(rows));
    frame.node.files.versions = {1};
    rows.row("its version moved -> " + frame.cooked(rows));
    frame.bound("one.obj");
    frame.node.files.versions = {1, 1}; // the file open still, as the engine shares it
    rows.row("bound again, the same file -> " + frame.cooked(rows));
    Files::save((frame.folder / "one.obj").string(), broken);
    frame.node.files.versions = {2, 2};
    rows.row("saved naming vertex 9 of 3 -> " + frame.cooked(rows));
    rows.row("and the frame after -> " + frame.cooked(rows));
    Framed small(rows, "square.obj", "1");
    rows.row("square.obj, most 1 -> " + small.cooked(rows));
    small.bound("square.obj", "2");
    rows.row("most set to 2 -> " + small.cooked(rows));
    Framed none(rows, "");
    rows.row("no file -> " + none.cooked(rows));
    Framed gone(rows, "gone.obj");
    rows.row("a file not there -> " + gone.cooked(rows));
  });

  // Another file named in its place, as a param set binds it: the file's version is its
  // own, so the one the node parsed last says nothing of it.
  bench.check("MeshFile.cpp/MeshFile::cook, another file named", [](Rows &rows) {
    Framed frame(rows, "one.obj");
    rows.row("one.obj named -> " + frame.cooked(rows));
    frame.bound("square.obj");
    rows.row("square.obj named in its place -> " + frame.cooked(rows));
    frame.bound("broken.obj");
    rows.row("broken.obj named in its place -> " + frame.cooked(rows));
  });
}

} // namespace VP::bench
