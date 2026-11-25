#include "runtime/Bound.h"

#include "tools/bench/Bench.h"
#include "tools/bench/Worlds.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// runtime/Bound.h: what the schedule's three files share. A param's bytes are written
// into a block of 0x77s, so a write past the type, or before a refusal, shows; the draw
// rules run on the stage files a folder may hold.

namespace VP::bench {

namespace domain {
struct GlslType {};
struct Stages {};
} // namespace domain

template <> struct Gen<domain::GlslType> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"float",
            "",
            "int",
            "uint",
            "vec2",
            "vec3",
            "vec4",
            "ivec2",
            "ivec3",
            "ivec4",
            "uvec2",
            "uvec3",
            "uvec4",
            "mat4",
            "double",
            "Float",
            "Texture"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::Stages> {
  using type = std::vector<std::string>;
  static std::vector<type> edges() {
    return {{"A.vert", "A.frag"},
            {},
            {"A.comp"},
            {"A.vert"},
            {"A.frag"},
            {"A.vert", "A.geom", "A.frag"},
            {"A.vert", "A.tesc", "A.tese", "A.frag"},
            {"A.vert", "A.tesc", "A.frag"},
            {"A.geom"},
            {"A.comp", "A.cpp"},
            {"A.cpp"},
            {"A.vert", "A.frag", "A.cpp"}};
  }
  static type random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

namespace {

// The 16 bytes of a block after a write, in hex, as many as a vec4 takes.
constexpr std::size_t block_bytes = 16;
constexpr std::byte untouched{0x77};

std::string written(std::string_view text, std::string_view type) {
  std::array<std::byte, block_bytes> block;
  block.fill(untouched);
  const bool parsed = write_value(text, type, block.data());
  std::string bytes;
  for (const std::byte byte : block)
    bytes += std::format("{:02x}", static_cast<unsigned>(byte));
  return std::format("{} {}", parsed, bytes);
}

std::string drawn(const std::vector<std::string> &files) {
  const Node node{.name = "n", .files = files};
  return std::format("vertex {} fragment {} draw {} lacks {} operator {}",
                     is_vertex(node),
                     is_fragment(node),
                     is_draw(node),
                     lacks_stages(node),
                     runs_operator(node));
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Bound.h/write_value", written, domain::Word{}, domain::GlslType{});
  bench.holds(
      "Bound.h/write_value",
      "a refused text writes no byte",
      [](std::string_view text, std::string_view type) {
        std::array<std::byte, block_bytes> block;
        block.fill(untouched);
        return write_value(text, type, block.data()) ||
               std::ranges::all_of(block,
                                   [](std::byte byte) { return byte == untouched; });
      },
      domain::Word{},
      domain::GlslType{});

  bench.sweep("Bound.h/is_draw and the stage rules", drawn, domain::Stages{});
  bench.sweep(
      "Bound.h/instance_number",
      [](std::string_view count) {
        return instance_number(Node{.name = "n", .instance_count = std::string(count)});
      },
      domain::Word{});
  bench.sweep(
      "Bound.h/pixel_format",
      [](std::string_view name) {
        const PixelFormat *format = pixel_format(name);
        return format ? std::format("{} {} bytes", format->pixel, format->bytes) : "none";
      },
      domain::Name{});
  bench.sweep(
      "Bound.h/as_word",
      [](std::string_view key, std::string_view value) {
        return as_word("n", Param{std::string(key), std::string(value)});
      },
      domain::Name{},
      domain::Word{});
  bench.sweep(
      "Bound.h/joining",
      [](const View &view, std::string_view writer) {
        const std::vector<std::string> readers{"b.in", "c.in"};
        return joining(view, writer, readers);
      },
      domain::Type{},
      domain::NodePath{});
  bench.sweep("Bound.h/smooth", [](float took, std::uint8_t frames) {
    float smoothed = 0;
    float last = 0;
    bool known = false;
    for (std::uint8_t frame = 0; frame <= frames % 120; ++frame)
      smooth(smoothed, last, known, frame == 0 ? 0.0f : took);
    return std::pair{smoothed, last};
  });
  bench.sweep(
      "Bound.h/disagreements",
      [](std::string_view cpp_type,
         std::uint32_t cpp_stride,
         std::string_view glsl_type,
         std::uint32_t stride) {
        const Field cpp{.name = "p", .type = std::string(cpp_type), .stride = cpp_stride};
        const Field glsl{.name = "p", .type = std::string(glsl_type), .stride = stride};
        return disagreements(cpp, glsl, "writes");
      },
      domain::GlslType{},
      domain::Type{},
      domain::GlslType{});

  // A connection of the view holds the port's name already, which the sweep's views never
  // do: the line names the writer's node too.
  bench.check("Bound.h/joining, its name taken", [](Rows &rows) {
    View view;
    view.connections.push_back(Connection{.name = "b"});
    const std::vector<std::string> readers{"c.in"};
    for (const std::string_view writer : {"a.b", "x.a.b", "b"})
      rows.row(std::format("{} -> {}", writer, joining(view, writer, readers)));
  });

  // Whether a node's code was saved since the build made what it binds, a shader since
  // its SPIR-V or a .cpp since the node's module, as a pull while vulpen runs: the node
  // then waits for the build rather than bind what is older than its code.
  bench.check("Bound.h/saved_since_built", [](Rows &rows) {
    const std::filesystem::path folder = rows.root() / "saved";
    Files::save(folder / "a.comp", "void main() {}");
    Files::save(folder / "n.cpp", "int n;");
    const std::filesystem::file_time_type saved =
        std::filesystem::last_write_time(folder / "a.comp");
    std::filesystem::last_write_time(folder / "n.cpp", saved);
    const Node node{.name = "n", .files = {"a.comp", "n.cpp"}, .folder = folder};
    Bound bound;
    bound.node = &node;
    bound.shaders_named = {"a.comp"};
    using std::chrono::seconds;
    const auto since = [&](seconds shader, std::optional<seconds> module) {
      bound.built = {saved + shader};
      bound.module_built.reset();
      if (module)
        bound.module_built = saved + *module;
      return saved_since_built(bound);
    };
    rows.row(
        std::format("both built after their saves -> {}", since(seconds(1), seconds(1))));
    rows.row(std::format("the shader saved since its SPIR-V -> {}",
                         since(seconds(-60), seconds(1))));
    rows.row(std::format("the .cpp saved since the module -> {}",
                         since(seconds(1), seconds(-60))));
    rows.row(std::format("linked in, no module -> {}", since(seconds(1), std::nullopt)));
  });
  bench.leaves_out(
      "Bound.h/saved_since_built",
      "release",
      "a release build links its nodes in, so nothing of it waits for a build");
}

} // namespace VP::bench
