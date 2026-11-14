#include "Class.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Fields.h"
#include "shared/Words.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The pass block as every start writes it, so one made reads as one uncommented.
constexpr std::string_view block_open = "layout(set = 1, binding = 0) uniform Pass {";
constexpr std::string_view block_close = "} pass;";
constexpr std::string_view field_indent = "  ";
// Where a view keeps what two of its nodes share (RV05), as an include names it.
constexpr std::string_view contracts = "contracts";
// A buffer type made for a reader is named for its element, as a class names it, plural,
// and this after it: Vec4sIn of vec4, as GpuLayout's FloatsIn of float.
constexpr std::string_view reads_suffix = "sIn";

bool stage_file(const std::filesystem::path &file) {
  return std::ranges::find(VP::stages, file.extension().string()) != VP::stages.end();
}

// A struct's type, as C++ and GLSL name one here, CamelCase (RC01), which only a
// definition gives a shader, where GLSL's own types are lowercase.
bool struct_type(std::string_view type) {
  return !type.empty() && type.front() >= 'A' && type.front() <= 'Z';
}

// A shader as a line changes its rows, and where its pass block is among them: its first
// row and its last, none for a shader with none, and whether it is live or, as a start's,
// commented out for a person to uncomment.
struct Source : Code {
  std::optional<std::size_t> open;
  std::size_t close = 0;
  bool live = false;

  bool commented(std::size_t at) const {
    return trim(rows[at]).starts_with("//");
  }
  // A row's code, a commented-out one's as it was before.
  std::string code(std::size_t at) const {
    return uncommented(rows[at]).value_or(rows[at]);
  }
  // Its block's fields, the live ones or those commented out, by their rows.
  std::vector<std::pair<std::size_t, Declared>> fields(bool live_ones) const {
    std::vector<std::pair<std::size_t, Declared>> found;
    if (!open)
      return found;
    for (std::size_t at = *open + 1; at < close; ++at)
      if (commented(at) != live_ones)
        if (std::optional<Declared> field = field_of(code(at)))
          found.emplace_back(at, std::move(*field));
    return found;
  }
  // The fields its live block holds, in their order, which every shader of a node
  // declaring one holds alike.
  std::vector<Declared> held() const {
    std::vector<Declared> fields;
    if (live)
      for (auto &[at, field] : this->fields(true))
        fields.push_back(std::move(field));
    return fields;
  }
  void insert(std::size_t at, std::string row) {
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(at), std::move(row));
    if (open && at <= *open)
      ++*open;
    if (open && at <= close)
      ++close;
  }
  void erase(std::size_t at) {
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(at));
    if (open && at < close)
      --close;
  }
};

// Its block, a live one first, as a start's commented out stays once a person wrote one
// of their own; one not closed is none.
void find_block(Source &source) {
  for (const bool live : {true, false})
    for (std::size_t at = 0; at < source.rows.size(); ++at) {
      if (source.commented(at) == live || !opens_block(source.code(at)))
        continue;
      for (std::size_t end = at + 1; end < source.rows.size(); ++end)
        if (source.commented(end) != live && trim(source.code(end)).starts_with('}')) {
          source.open = at;
          source.close = end;
          source.live = live;
          return;
        }
    }
}

Source read_source(VP::FilePort &files, const std::filesystem::path &file) {
  Source source;
  static_cast<Code &>(source) = read_code(files, file);
  find_block(source);
  return source;
}

// Where what a block holds is declared by: past the last row before any code that
// includes a file or declares a buffer, so a row a line adds comes where a start has its
// own.
std::size_t head_end(const Rows &rows) {
  std::size_t end = 0;
  for (std::size_t at = 0; at < rows.size(); ++at) {
    const std::string_view code = trim(rows[at]);
    if (code.starts_with('#') || buffer_declared(rows[at], false))
      end = at + 1;
    else if (!code.empty() && !code.starts_with("//"))
      break;
  }
  return end;
}

// A start's hint row becomes the field, at its indent, and the rows its hint goes on in
// go with it: the person's use of the field is their own now.
void take_hint(Source &source, std::size_t hint, const Declared &field) {
  const std::string code = source.code(hint);
  source.rows[hint] = std::format(
      "{}{} {};", code.substr(0, code.find_first_not_of(" \t")), field.type, field.name);
  while (hint + 1 < source.close && source.commented(hint + 1) &&
         trim(source.code(hint + 1)).starts_with("//"))
    source.erase(hint + 1);
}

// Declares a field in a shader's pass block: a start's row of its type commented out
// becomes it, else a row of its own after the live ones does; shared, only a row after
// them, so a node's shaders hold their fields in one order. A block commented out is
// uncommented first, and a shader with none gets one after what its rows declare.
// copied: the fields the other shaders of its node hold, which a block holding none yet
// holds first, as they share one.
void declare(Source &source,
             std::span<const Declared> copied,
             const Declared &field,
             bool shared) {
  const auto row = [](const Declared &each) {
    return std::format("{}{} {};", field_indent, each.type, each.name);
  };
  if (!source.open) {
    Rows block{std::string(block_open)};
    std::ranges::transform(copied, std::back_inserter(block), row);
    block.push_back(row(field));
    block.emplace_back(block_close);
    std::size_t at = head_end(source.rows);
    if (at < source.rows.size() && !trim(source.rows[at]).empty())
      block.emplace_back();
    for (std::string &each : block)
      source.insert(at++, std::move(each));
    return;
  }
  if (!source.live) {
    source.rows[*source.open] = source.code(*source.open);
    source.rows[source.close] = source.code(source.close);
    source.live = true;
  }
  const std::vector<std::pair<std::size_t, Declared>> held = source.fields(true);
  std::size_t at = held.empty() ? *source.open + 1 : held.back().first + 1;
  for (const Declared &each : copied)
    source.insert(at++, row(each));
  for (std::size_t hint = shared ? at : *source.open + 1; hint < source.close; ++hint)
    if (const std::optional<Declared> start =
            source.commented(hint) ? field_of(source.code(hint)) : std::nullopt;
        start && start->type == field.type)
      return take_hint(source, hint, field);
  source.insert(at, row(field));
}

// GpuLayout.glsl declares Texture, Image, its buffers and the macros that declare the
// others, so it comes before any row that uses them: first of the includes.
void include_layout(Source &source) {
  if (std::ranges::any_of(source.rows, [](const std::string &row) {
        return include_of(row) == gpu_layout;
      }))
    return;
  const auto first = std::ranges::find_if(
      source.rows, [](const std::string &row) { return include_of(row).has_value(); });
  source.insert(first == source.rows.end()
                    ? head_end(source.rows)
                    : static_cast<std::size_t>(first - source.rows.begin()),
                std::format("#include \"{}\"", gpu_layout));
}

// Gives a shader the type a field of it holds, where it lacks it: GpuLayout.glsl for any
// but a value, then for another buffer type its own row declaring it uncommented, as a
// start's hint, else the rows given; false when it lacks it and none are given.
bool give_type(VP::FilePort &files,
               Source &source,
               std::string_view type,
               const Rows &given) {
  if (std::ranges::find(value_types, type, &ValueType::glsl) != value_types.end())
    return true;
  include_layout(source);
  const std::vector<BufferType> buffers = buffers_of(files, source.file, source.text());
  const auto held = std::ranges::find(buffers, type, &BufferType::name);
  if (type == VP::texture_type || type == VP::image_type ||
      (held != buffers.end() && !held->commented))
    return true;
  for (std::size_t at = 0; held != buffers.end() && at < source.rows.size(); ++at)
    if (const std::optional<BufferType> hint = buffer_declared(source.rows[at], true);
        hint && hint->name == type) {
      source.rows[at] = source.code(at);
      return true;
    }
  for (const std::string &row : given)
    if (std::ranges::none_of(source.rows, [&](const std::string &own) {
          return own == row || (include_of(row) && include_of(own) == include_of(row));
        }))
      source.insert(head_end(source.rows), row);
  return !given.empty();
}

// Whether a row starts the struct of that name, as `struct Corner {`.
bool declares_struct(std::string_view row, std::string_view type) {
  const std::string_view code = trim(row);
  const std::string declared = std::format("struct {}", type);
  return code.starts_with(declared) &&
         (code.size() == declared.size() || code[declared.size()] == ' ' ||
          code[declared.size()] == '{');
}

// The include of a shader's that declares a type: a struct, or a buffer type.
std::optional<std::string>
including(VP::FilePort &files, const Source &source, std::string_view declared) {
  for (const std::string &row : source.rows) {
    const std::optional<std::string> include = include_of(row);
    const std::optional<std::filesystem::path> path =
        include ? resolved_include(files, source.file, *include) : std::nullopt;
    if (!path)
      continue;
    for (const std::string &included : rows_of_text(files.read(path->string())))
      if (const std::optional<BufferType> buffer = buffer_declared(included, false);
          (buffer && buffer->name == declared) || declares_struct(included, declared))
        return row;
  }
  return std::nullopt;
}

// The rows a shader declares a buffer type by, for another of its node that lacks it: the
// include declaring it, or its own row, after the include declaring its element's struct;
// none for a struct it declares itself, which a contract both include holds instead
// (RV05).
Rows declaring(VP::FilePort &files, const Source &source, std::string_view type) {
  if (const std::optional<std::string> include = including(files, source, type))
    return {*include};
  for (const std::string &row : source.rows)
    if (const std::optional<BufferType> buffer = buffer_declared(row, false);
        buffer && buffer->name == type) {
      if (!struct_type(buffer->element))
        return {row};
      if (const std::optional<std::string> include =
              including(files, source, buffer->element))
        return {*include, row};
    }
  return {};
}

// The contract declaring a struct: in the contracts/ of the nearest folder above the
// shader whose contracts/ declares it, as a view's top holds its own (RV05).
std::optional<std::filesystem::path> contract_of(VP::FilePort &files,
                                                 const std::filesystem::path &file,
                                                 std::string_view type) {
  for (std::filesystem::path at = file.parent_path();; at = at.parent_path()) {
    for (const std::string &name : files.list((at / contracts).string()))
      for (const std::string &row :
           name.ends_with(".glsl")
               ? rows_of_text(files.read((at / contracts / name).string()))
               : Rows())
        if (declares_struct(row, type))
          return at / contracts / name;
    if (at == at.parent_path())
      return std::nullopt;
  }
}

// The buffer type a shader reads a writer's buffer by, and the rows that declare it where
// it lacks them: one reading the element it holds, GpuLayout's FloatsIn for floats, or
// one of the shader's own, live or commented out; else, for a struct, the contract
// declaring it, with its reading type, as Corners of Corner; else one named for the
// element, as Vec4sIn of vec4, which a row of its own declares.
std::pair<std::string, Rows>
reader_type(VP::FilePort &files, const Source &source, std::string_view element) {
  for (const BufferType &buffer : buffers_of(files, source.file, source.text()))
    if (buffer.element == element && buffer.reads && !buffer.writes)
      return {buffer.name, {}};
  Rows rows;
  if (struct_type(element)) {
    const std::optional<std::filesystem::path> contract =
        contract_of(files, source.file, element);
    if (!contract)
      throw std::runtime_error(std::format(
          "{} is declared in no contract a shader of {} may include: a contracts/ folder "
          "holds what two nodes share (RV05)",
          element,
          shown(source.file.parent_path())));
    rows.push_back(
        std::format("#include \"{}/{}\"", contracts, contract->filename().string()));
    for (const std::string &row : rows_of_text(files.read(contract->string())))
      if (const std::optional<BufferType> buffer = buffer_declared(row, false);
          buffer && buffer->element == element && buffer->reads && !buffer->writes)
        return {buffer->name, rows};
  }
  const std::string name = class_of(element) + std::string(reads_suffix);
  rows.push_back(std::format("VP_READS({}, {});", name, element));
  return {name, rows};
}

// The file holding a shader's pass block: its own, live or commented out, else a file
// beside it that it includes holding a live one, as a part's shaders share theirs so;
// else its own, where one is made.
Source holder(VP::FilePort &files, const std::filesystem::path &file) {
  Source source = read_source(files, file);
  if (source.open)
    return source;
  for (const std::string &row : source.rows)
    if (const std::optional<std::string> include = include_of(row))
      if (const std::filesystem::path beside =
              (file.parent_path() / *include).lexically_normal();
          listed_file(files, beside))
        if (Source held = read_source(files, beside); held.open && held.live)
          return held;
  return source;
}

// The other shaders of its folder, its node's, that declare a live pass block of their
// own, which theirs shares, as the engine reads one for a node.
std::vector<Source> siblings(VP::FilePort &files,
                             const std::filesystem::path &file,
                             const std::filesystem::path &holding) {
  std::vector<Source> found;
  for (const std::string &name : files.list(file.parent_path().string()))
    if (const std::filesystem::path path = file.parent_path() / name;
        path != file && path != holding && stage_file(path))
      if (Source source = read_source(files, path); source.open && source.live)
        found.push_back(std::move(source));
  return found;
}

[[noreturn]] void differ(const Source &one, const Source &other) {
  throw std::runtime_error(std::format(
      "{} and {} declare different pass blocks, which their node refuses: make "
      "them alike first, or declare one block in a file both include",
      shown(one.file),
      shown(other.file)));
}

// What a declare line writes, all known before a file is written, so a line refused
// writes none (A02): the field in the shader's pass block, or in the file it includes
// that holds it, and in each other shader of its node that declares one, as a node's
// shaders share one; a shader holding none yet first takes the fields theirs hold. given:
// the rows that declare the field's type where a shader lacks it.
std::vector<Source> declaring(VP::FilePort &files,
                              const std::filesystem::path &file,
                              const Declared &field,
                              const Rows &given) {
  Source target = holder(files, file);
  std::vector<Source> others = siblings(files, file, target.file);
  const std::vector<Declared> mine = target.held();
  const std::vector<Declared> theirs =
      others.empty() ? std::vector<Declared>() : others.front().held();
  for (const Source &other : others)
    if (other.held() != theirs)
      differ(others.front(), other);
  if (!others.empty() && !mine.empty() && mine != theirs)
    differ(target, others.front());
  const std::vector<Declared> &holds = mine.empty() ? theirs : mine;
  if (const auto had = std::ranges::find(holds, field.name, &Declared::name);
      had != holds.end())
    throw std::runtime_error(std::format("the pass block of {} holds {} already, a {}",
                                         shown(target.file),
                                         field.name,
                                         had->type));
  const std::vector<Declared> copied = mine.empty() ? theirs : std::vector<Declared>();
  declare(target, copied, field, !others.empty());
  for (const Declared &each : copied)
    if (!give_type(files, target, each.type, declaring(files, others.front(), each.type)))
      throw std::runtime_error(std::format(
          "{} declares {} in a way {} cannot take over: declare it in a contract both "
          "include (RV05)",
          shown(others.front().file),
          each.type,
          shown(target.file)));
  give_type(files, target, field.type, given);
  for (Source &other : others) {
    declare(other, {}, field, true);
    give_type(files,
              other,
              field.type,
              given.empty() ? declaring(files, target, field.type) : given);
  }
  others.insert(others.begin(), std::move(target));
  return others;
}

// The files a line wrote, as its answer names them: the shader's, or the file it includes
// holding its block, then the other shaders of its node.
std::string named(std::span<const Source> written, const std::filesystem::path &file) {
  std::string text = shown(written.front().file);
  for (const Source &other : written.subspan(1))
    text += std::format(" and {}", shown(other.file));
  if (written.size() > 1)
    text += ", which share their node's pass block";
  if (written.front().file != file)
    text += std::format(", which {} includes", shown(file));
  return text;
}

// A file that may declare a field or a port: a shader there, but a lone .frag, which
// reads its .vert's (A02), or a .cpp there, whose class declares them.
void check_file(VP::FilePort &files, const std::filesystem::path &file) {
  if (!stage_file(file) && file.extension() != cpp_extension)
    throw std::runtime_error(
        std::format("{} is no shader or .cpp: a .vert, .tesc, .tese, .geom, .frag or "
                    ".comp declares a pass block, a .cpp a class",
                    shown(file)));
  if (!listed_file(files, file))
    throw std::runtime_error(std::format("{} is not there", shown(file)));
  if (stage_file(file) && lone_fragment(files.list(file.parent_path().string())))
    throw std::runtime_error(std::format(
        "{} is its folder's only draw stage, which reads the pass block of the .vert it "
        "draws with and declares none: declare it there, or new {} makes its node a draw "
        "of its own",
        shown(file),
        shown(file.parent_path() /
              (file.stem().string() + std::string(VP::vertex_stage)))));
}

// A node's param or port added from inside vulpen, by lines a menu sends too (V06): a
// field written into a shader's pass block, the one source of what the node holds (RA03),
// or a line in its C++'s bind() and the member it assigns, which the live build then
// builds and the bind reads, so param set and connect find it as they find any. A value
// nothing sets reads 0, and says the line that sets it; a Texture nothing fills samples
// black, and says the line that connects it.
class Declare final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _param = node.command(
        "param declare <file> <type> [<name>]",
        "writes a field into a shader's pass block, and into each other shader of its "
        "node that declares one, as they share it: a value a param sets, a Texture, an "
        "Image or a buffer, as Tab lists them; a start's block and its row of that type "
        "uncommented, renamed, else written. Into a .cpp, a line in bind() and a member "
        "of the class its VP_OPERATOR names: a param of a value or a string, an output "
        "of a type its view's contracts declare, or, named alone, a field of its node's "
        "shaders nothing fills yet, which its cook fills or reads back; a start's line "
        "of that way uncommented, renamed, else written");
    _connect = node.command(
        "connect declare <port> <file> <name>",
        "writes the field that reads what a port writes into a shader's pass block, as "
        "param declare does, a Texture for an image and a buffer of its element for a "
        "buffer, or into a .cpp an input of the contract a C++ output carries, then "
        "connects the port to it, as connect does");
  }

  void command(VP::Call &call) override {
    const std::span<const std::string_view> words = call.arguments();
    if (call.is(_param))
      param(call, words[0], words[1], words.size() > 2 ? words[2] : std::string_view());
    else if (call.is(_connect))
      connect(call, words[0], words[1], words[2]);
  }

  static void param(VP::Call &call,
                    std::string_view path,
                    std::string_view type,
                    std::string_view name) {
    VP::FilePort &files = call.files();
    const std::filesystem::path file(path);
    check_file(files, file);
    if (file.extension() == cpp_extension)
      return cpp_param(call, file, type, name);
    if (name.empty())
      throw std::runtime_error(std::format(
          "a shader's field takes a type and a name: param declare {} {} <name>",
          shown(file),
          type));
    const std::vector<std::string> types = field_types(files, file);
    if (std::ranges::find(types, type) == types.end())
      throw std::runtime_error(
          std::format("{} is no type {} may declare; it may declare {}",
                      type,
                      shown(file),
                      listed(types)));
    check_name(name);
    const std::vector<Source> written =
        declaring(files, file, {std::string(type), std::string(name)}, {});
    for (const Source &source : written)
      save(files, source);
    call.reply(std::format("declared {} {} in {}", type, name, named(written, file)));
  }

  // A .cpp's param, an output, or, named alone, a field of its node's shaders: a line in
  // bind() and a member, which the live build then builds.
  static void cpp_param(VP::Call &call,
                        const std::filesystem::path &file,
                        std::string_view type,
                        std::string_view name) {
    VP::FilePort &files = call.files();
    const Binding binding = name.empty() ? field_binding(files, call.view(), file, type)
                                         : typed_binding(files, file, type, name);
    const std::vector<Code> written = declaring_cpp(files, file, binding);
    for (const Code &code : written)
      save(files, code);
    call.reply(std::format("declared {} {} in {}, {}",
                           binding.said,
                           binding.name,
                           named(written),
                           next_use(binding, node_named(call.view(), file))));
  }

  static void connect(VP::Call &call,
                      std::string_view port,
                      std::string_view path,
                      std::string_view name) {
    VP::FilePort &files = call.files();
    const std::filesystem::path file(path);
    check_file(files, file);
    check_name(name);
    const VP::View &view = call.view();
    const std::string line =
        joining_line(view, port, std::format("{}.{}", node_holding(view, file), name));
    if (file.extension() == cpp_extension) {
      const Binding binding = input_binding(call, file, port, name);
      const std::vector<Code> written = declaring_cpp(files, file, binding);
      return connect_written(
          call,
          written,
          line,
          std::format("declared {} {} in {}", binding.said, name, named(written)));
    }
    const auto [type, given] = read_as(call, holder(files, file), port);
    const std::vector<Source> written =
        declaring(files, file, {type, std::string(name)}, given);
    connect_written(
        call,
        std::vector<Code>(written.begin(), written.end()),
        line,
        std::format("declared {} {} in {}", type, name, named(written, file)));
  }

  // Connected as connect does, once the files are written, so a line refused there puts
  // them back as they were and changes nothing.
  static void connect_written(VP::Call &call,
                              const std::vector<Code> &written,
                              const std::string &line,
                              const std::string &declared) {
    for (const Code &code : written)
      save(call.files(), code);
    try {
      std::string connected = call.commands().send(line);
      while (connected.ends_with('\n'))
        connected.pop_back();
      call.reply(std::format("{}; {}", declared, connected));
    } catch (const std::runtime_error &) {
      for (const Code &code : written)
        put_back(call.files(), code);
      throw;
    }
  }

  // The type a shader reads a port by, as the bind found what the port writes, and the
  // rows declaring it: a Texture for an image or a view's window, a buffer of the element
  // a buffer holds; refused for any other port, naming those it may be (A02).
  static std::pair<std::string, Rows>
  read_as(VP::Call &call, const Source &source, std::string_view port) {
    if (port.ends_with(':'))
      return {std::string(VP::texture_type), {}};
    std::vector<std::string> writers;
    for (const VP::NodePort &each : call.ports()) {
      const bool data = each.kind == "image" || each.kind == "buffer";
      if (!each.writes || std::format("{}.{}", each.node, each.name) != port) {
        if (each.writes && data)
          writers.push_back(std::format("{}.{}", each.node, each.name));
        continue;
      }
      if (each.kind == "image")
        return {std::string(VP::texture_type), {}};
      if (each.kind == "buffer" && !each.holds.empty())
        return reader_type(call.files(), source, each.holds);
      throw std::runtime_error(std::format(
          "{} {}, which no shader reads by its pass block{}",
          port,
          each.kind == "object" ? "carries a C++ object" : "names no element",
          each.kind == "object"
              ? std::format(": connect declare {} <its .cpp> <name> reads it in C++",
                            port)
              : ""));
    }
    throw std::runtime_error(
        std::format("{} is no image or buffer a node of view {} writes; they are {}",
                    port,
                    call.view().name,
                    writers.empty() ? std::string("none") : listed(writers)));
  }

  // A writer a connection joins already takes the reader into it by its name, as a port
  // joins one connection; else the line leaves the name out, for connect to choose.
  static std::string
  joining_line(const VP::View &view, std::string_view port, const std::string &reader) {
    const auto joined =
        std::ranges::find_if(view.connections, [&](const VP::Connection &each) {
          return each.from.text() == port;
        });
    return joined == view.connections.end()
               ? std::format("connect {} {}", port, reader)
               : std::format("connect {} {} {}", joined->name, port, reader);
  }

  VP::Command _param;
  VP::Command _connect;
};

} // namespace

VP_OPERATOR(Declare);
