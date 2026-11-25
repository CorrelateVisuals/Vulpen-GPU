#include "shared/Fields.h"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's shared/Fields.h: what a shader's pass block may hold, and what a node's
// C++ may declare, read alike by param declare, the terminal's Tab and the menus. The
// files are a view in the sandbox: a compute node whose shader declares buffers and
// includes a contract, a draw whose two shaders share a block one includes, a lone
// fragment, and a C++ contract.

namespace VP::bench {

template <> struct Shown<::Declared> {
  static std::string text(const ::Declared &field, Names &) {
    return std::format("{} {}", field.type, field.name);
  }
};
template <> struct Shown<::BufferType> {
  static std::string text(const ::BufferType &buffer, Names &) {
    return std::format("{} of {}{}{}{}",
                       buffer.name,
                       buffer.element,
                       buffer.reads ? " reads" : "",
                       buffer.writes ? " writes" : "",
                       buffer.commented ? " commented" : "");
  }
};
template <> struct Shown<::CppField> {
  static std::string text(const ::CppField &each, Names &names) {
    return std::format("{}{}",
                       show(each.field, names),
                       each.taken.empty() ? "" : " (" + each.taken + ")");
  }
};
template <> struct Shown<::CppContract> {
  static std::string text(const ::CppContract &contract, Names &) {
    return std::format("{} in {}", contract.type, contract.header);
  }
};

namespace {

using worlds::connection;
using worlds::node;

constexpr std::string_view compute = R"(#include "baseclasses/GpuLayout.glsl"
#include "contracts/Corner.glsl"
#include "Missing.glsl"

layout(local_size_x = 64) in;

VP_WRITES(Totals, float);
// VP_BUFFER(Hints, vec4);
VP_READS( Bad ;

layout(set = 1, binding = 0) uniform Pass {
  float speed;   // from its C++
  Image target;  // stored into
  Corners corners;
  uint count;    // a param sets it
  // uint hidden;
  vec2 gl_place;
  uint steps[4];
  Totals totals; // read back
} pass;

void main() {
  float local;
}
)";

constexpr std::string_view corner = R"(struct Corner { vec2 at; };
VP_READS(Corners, Corner);
)";

constexpr std::string_view block = R"(#include "baseclasses/GpuLayout.glsl"
layout(set = 1, binding = 0) uniform Pass {
  vec4 tint;
  Texture picture;
} pass;
)";

constexpr std::string_view contract = R"(#pragma once
namespace VP_VIEW {
struct alignas(16) Spin {
  float angle;
};
using Spins = std::vector<Spin>;
struct Later;
// struct Hidden {
struct Base final {};
struct Derived : Base {};
}
)";

// The view's folder, planted: a file each, its text as given.
std::filesystem::path planted_view(const Rows &rows) {
  const std::filesystem::path top = rows.root() / "tree";
  for (const auto &[file, text] : std::vector<std::pair<std::string, std::string_view>>{
           {"view.vlp", ""},
           {"Top.cpp", ""},
           {"contracts/Corner.glsl", corner},
           {"contracts/Spin.h", contract},
           {"contracts/Notes.txt", "struct Not {};"},
           {"sum/Sum.comp", compute},
           {"sum/Sum.cpp",
            "const float speed = bind.param<float>(\"speed\");\n"
            "// read(\"totals\");\n"},
           {"sum/Sum.h", "// what Sum.cpp shares\n"},
           {"draw/Draw.vert", "#include \"Draw.glsl\"\n"},
           {"draw/Draw.frag", "#include \"Draw.glsl\"\n"},
           {"draw/Draw.glsl", block},
           {"draw/Draw.cpp", ""},
           {"lone/Lone.frag", "void main() {}\n"},
           {"lone/Lone.cpp", ""},
           {"bare/Bare.cpp", ""}})
    Files::save((top / file).string(), text);
  return top;
}

View view_of(const std::filesystem::path &top) {
  View view{.name = "tree", .file = top / "view.vlp"};
  view.nodes = {node(std::string(top_node)),
                node("sum", {{"count", "4"}}),
                node("draw"),
                node("lone"),
                node("source")};
  view.connections = {connection("corners", "source.out", {"sum.corners"})};
  return view;
}

} // namespace

void cases(Bench &bench) {
  bench.check("Fields.h/value_type", [](Rows &rows) {
    for (const ValueType &value : value_types)
      rows.row(std::format("{} as {}", value.glsl, value.cpp));
    rows.row(std::format("and a word: {} as {}", word_type.glsl, word_type.cpp));
  });
  bench.check("Fields.h/layout_buffers",
              [](Rows &rows) { rows.row(show(layout_buffers(), rows.names())); });

  bench.sweep("Fields.h/uncommented", uncommented, domain::FileText{});
  bench.check("Fields.h/uncommented", [](Rows &rows) {
    for (const std::string_view row : {"// VP_READS(A, float);",
                                       "  //VP_READS(A, float);",
                                       "//  two blanks",
                                       "code; // after",
                                       "/ / no",
                                       "",
                                       "//"})
      rows.row(
          std::format("{} -> {}", escaped(row), show(uncommented(row), rows.names())));
  });

  bench.sweep("Fields.h/buffer_declared", buffer_declared, domain::Word{});
  bench.check("Fields.h/buffer_declared", [](Rows &rows) {
    for (const std::string_view row : {"VP_READS(Corners, Corner);",
                                       "VP_WRITES(Totals, float);",
                                       "  VP_BUFFER( Both , uint ) ;",
                                       "// VP_BUFFER(Hints, vec4);",
                                       "VP_READS(Bad;",
                                       "VP_READS(, float);",
                                       "VP_READS(A);",
                                       "VP_READS(A, B, C);",
                                       "VP_READ(A, b);",
                                       "x VP_READS(A, b);",
                                       "VP_READS(A, b) // VP_WRITES(C, d)"})
      for (const bool commented : {false, true})
        rows.row(std::format("{}{} -> {}",
                             escaped(row),
                             commented ? " as a hint" : "",
                             show(buffer_declared(row, commented), rows.names())));
  });

  bench.sweep("Fields.h/include_of", include_of, domain::Word{});
  bench.check("Fields.h/include_of", [](Rows &rows) {
    for (const std::string_view row : {"#include \"a.glsl\"",
                                       "  #include \"contracts/Rect.h\"  // why",
                                       "#include <vector>",
                                       "// #include \"a.glsl\"",
                                       "#include \"open",
                                       "#include \"\"",
                                       "#  include \"a.glsl\""})
      rows.row(
          std::format("{} -> {}", escaped(row), show(include_of(row), rows.names())));
  });

  bench.check("Fields.h/listed_file", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    SandboxFiles files;
    for (const std::string_view file :
         {"sum/Sum.comp", "sum", "sum/Gone.comp", "gone/Sum.comp"})
      rows.row(std::format("{} -> {}", file, listed_file(files, top / file)));
  });

  bench.check("Fields.h/resolved_include", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    SandboxFiles files;
    for (const std::string_view include : {"Draw.glsl",
                                           "contracts/Corner.glsl",
                                           "Missing.glsl",
                                           "../draw/Draw.glsl",
                                           "contracts/Spin.h"})
      rows.row(
          std::format("{} from draw/Draw.frag -> {}",
                      include,
                      here(show(resolved_include(files, top / "draw/Draw.frag", include),
                                rows.names()),
                           top)));
  });

  bench.check("Fields.h/buffers_of", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    SandboxFiles files;
    for (const std::string_view file :
         {"sum/Sum.comp", "draw/Draw.frag", "lone/Lone.frag"})
      rows.row(std::format(
          "{} -> {}",
          file,
          show(buffers_of(files, top / file, files.read((top / file).string())),
               rows.names())));
  });

  bench.check("Fields.h/lone_fragment", [](Rows &rows) {
    for (const std::vector<std::string> &names :
         std::vector<std::vector<std::string>>{{"Lone.frag"},
                                               {"Lone.frag", "Lone.cpp"},
                                               {"A.vert", "A.frag"},
                                               {"A.comp", "A.frag"},
                                               {"A.geom", "A.frag"},
                                               {},
                                               {"A.vert"}})
      rows.row(std::format("{} -> {}", show(names, rows.names()), lone_fragment(names)));
  });

  bench.check("Fields.h/Declared::operator==", [](Rows &rows) {
    const ::Declared a{"float", "x"};
    rows.row(std::format("float x == float x: {}, == float y: {}, == uint x: {}",
                         a == ::Declared{"float", "x"},
                         a == ::Declared{"float", "y"},
                         a == ::Declared{"uint", "x"}));
  });

  bench.sweep("Fields.h/glsl_name", glsl_name, domain::Name{});
  bench.check("Fields.h/glsl_name", [](Rows &rows) {
    for (const std::string_view name :
         {"speed", "_x", "x9", "9x", "gl_x", "gl", "a__b", "a b", "", "Ab_9", "\xc3\xa9"})
      rows.row(std::format("{} -> {}", escaped(name), glsl_name(name)));
  });

  bench.sweep("Fields.h/field_of", field_of, domain::Word{});
  bench.check("Fields.h/field_of", [](Rows &rows) {
    for (const std::string_view code : {"float speed;",
                                        "  vec2   at ;  // why",
                                        "uint steps[4];",
                                        "highp float x;",
                                        "float;",
                                        "float x",
                                        "float x; // a; b",
                                        "vec2 gl_place;",
                                        "} pass;",
                                        "Texture picture; // one; two",
                                        "\tuint\tcount;",
                                        "float a, b;"})
      rows.row(
          std::format("{} -> {}", escaped(code), show(field_of(code), rows.names())));
  });

  bench.check("Fields.h/opens_block", [](Rows &rows) {
    for (const std::string_view code :
         {"layout(set = 1, binding = 0) uniform Pass {",
          "layout(std140) uniform Pass",
          "uniform Pass {",
          "layout(push_constant) uniform Frame {",
          "  layout(set = 1, binding = 0) uniform Passes {"})
      rows.row(std::format("{} -> {}", escaped(code), opens_block(code)));
  });

  bench.sweep("Fields.h/live_fields", live_fields, domain::FileText{});
  bench.check("Fields.h/live_fields", [](Rows &rows) {
    for (const std::string_view text :
         {compute,
          block,
          corner,
          std::string_view("layout(set = 1, binding = 0) uniform Pass {\n  float x;\n"),
          std::string_view("// layout(set = 1, binding = 0) uniform Pass {\n//  float "
                           "x;\n// } pass;\n"),
          std::string_view("layout(set = 1, binding = 0) uniform Pass { float x; } "
                           "pass;\nvoid main() {\n  "
                           "float local;\n}\n"),
          std::string_view("layout(set = 1, binding = 0) uniform Pass { float x;\n"
                           "  vec2 y;\n} pass;\n"),
          std::string_view("layout(set = 1, binding = 0) uniform Pass\n{ float x;\n"
                           "  vec2 y; }\npass;\n")})
      rows.row(
          std::format("{} -> {}", escaped(text), show(live_fields(text), rows.names())));
  });

  bench.check("Fields.h/node_fields", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    SandboxFiles files;
    for (const std::string_view folder : {"sum", "draw", "lone", "bare", "gone"}) {
      const ::NodeFields found = node_fields(files, top / folder);
      rows.row(std::format("{} -> {} {}",
                           folder,
                           here(show(found.shader, rows.names()), top),
                           show(found.fields, rows.names())));
    }
  });

  bench.check("Fields.h/node_named", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    const View view = view_of(top);
    for (const std::filesystem::path &file : {top / "sum/Sum.cpp",
                                              top / "Top.cpp",
                                              top / "sum/inner/X.cpp",
                                              top / "gone/X.cpp",
                                              top / "../Outside.cpp",
                                              top / "./sum/../draw/Draw.cpp",
                                              std::filesystem::path("sum/Sum.cpp")})
      rows.row(std::format("{} -> {}",
                           escaped(here(file.generic_string(), top)),
                           show(node_named(view, file), rows.names())));
  });

  bench.check("Fields.h/cpp_fields", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    const View view = view_of(top);
    SandboxFiles files;
    for (const std::string_view file :
         {"sum/Sum.cpp", "draw/Draw.cpp", "lone/Lone.cpp", "sum/Sum.h", "bare/Bare.cpp"})
      rows.row(std::format(
          "{} -> {}", file, show(cpp_fields(files, top / file, view), rows.names())));
  });
  bench.check("Fields.h/free_fields", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    const View view = view_of(top);
    SandboxFiles files;
    for (const std::string_view file : {"sum/Sum.cpp", "draw/Draw.cpp", "gone/Gone.cpp"})
      rows.row(std::format(
          "{} -> {}", file, show(free_fields(files, top / file, view), rows.names())));
  });

  bench.sweep("Fields.h/contract_type", contract_type, domain::Word{});
  bench.check("Fields.h/contract_type", [](Rows &rows) {
    for (const std::string_view row : {"struct Spin {",
                                       "struct alignas(16) Spin {",
                                       "using Spins = std::vector<Spin>;",
                                       "struct Later;",
                                       "// struct Hidden {",
                                       "  struct Inner {",
                                       "struct Base final {};",
                                       "struct Derived : Base {",
                                       "struct alignas(16 Broken {",
                                       "using namespace std;",
                                       "struct 9x {",
                                       "struct {",
                                       "struct Spin",
                                       "using Spin= int;"})
      rows.row(
          std::format("{} -> {}", escaped(row), show(contract_type(row), rows.names())));
  });

  bench.check("Fields.h/cpp_contracts", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    SandboxFiles files;
    for (const std::string_view file : {"sum/Sum.cpp", "Top.cpp", "contracts/Spin.h"})
      rows.row(std::format(
          "{} -> {}", file, show(cpp_contracts(files, top / file), rows.names())));
    // Beside the view, where no folder up to the root holds contracts/.
    rows.row(std::format(
        "a file beside the view -> {}",
        show(cpp_contracts(files, rows.root() / "beside/Beside.cpp"), rows.names())));
  });

  bench.check("Fields.h/field_types", [](Rows &rows) {
    const std::filesystem::path top = planted_view(rows);
    SandboxFiles files;
    for (const std::string_view file : {"sum/Sum.comp",
                                        "draw/Draw.vert",
                                        "draw/Draw.frag",
                                        "lone/Lone.frag",
                                        "sum/Sum.cpp",
                                        "sum/Gone.comp",
                                        "sum/Sum.h",
                                        "view.vlp"})
      rows.row(std::format(
          "{} -> {}", file, show(field_types(files, top / file), rows.names())));
  });
}

} // namespace VP::bench
