#pragma once

// What a line declaring a param or a port in a node's C++ writes there: a bind() line and
// a member, by one name, in the class its VP_OPERATOR names, and what each type or field
// gives it, kept apart from what the lines write into shaders, so neither file grows past
// what one reads whole (CPP12); first, what both write by.

#include "runtime/Operator.h"
#include "shared/Fields.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// "And declared to be the Son of God with power, according to the spirit of holiness, by
// the resurrection from the dead:" Romans 1:4

// What a node's C++ names a contract's type by: the namespace the build gives each view
// (RV05), as code spells it.
constexpr std::string_view view_namespace = "VP_VIEW::";
// An operator's class, as a .cpp's VP_OPERATOR(Name) names it.
constexpr std::string_view operator_macro = "VP_OPERATOR(";
// The indent clang-format gives a row in a body, past its opening row's.
constexpr std::string_view body_indent = "  ";
// The pixel a Texture C++ fills takes when its node's image word names no format,
// R8G8B8A8_UNORM's, as Bind::texture checks it.
constexpr std::string_view default_pixel = "glm::u8vec4";

// The rows a line changes.
using Rows = std::vector<std::string>;

// A file as a line changes its rows, and as it read them, which a line refused after it
// wrote puts back.
struct Code {
  std::filesystem::path file;
  std::string read;
  Rows rows;

  std::string text() const {
    std::string text;
    for (std::size_t at = 0; at < rows.size(); ++at)
      text.append(at == 0 ? "" : "\n").append(rows[at]);
    return text;
  }
};

Code read_code(VP::FilePort &files, const std::filesystem::path &file) {
  Code code{.file = file, .read = files.read(file.string())};
  code.rows = rows_of_text(code.read);
  return code;
}

// A path as the prompt shows a folder, from where vulpen started, as the files part names
// one.
std::string shown(const std::filesystem::path &path) {
  return path.lexically_proximate(std::filesystem::current_path()).generic_string();
}

std::string listed(std::span<const std::string> words) {
  std::string text;
  for (const std::string &word : words)
    text.append(text.empty() ? "" : ", ").append(word);
  return text;
}

void check_name(std::string_view name) {
  if (!glsl_name(name))
    throw std::runtime_error(
        std::format("{} is no name a field takes: a letter or _ "
                    "first, then letters, digits and _, no gl_ first",
                    name));
}

// The files a line wrote into a node's C++, as its answer names them.
std::string named(std::span<const Code> written) {
  std::string text;
  for (const Code &code : written)
    text.append(text.empty() ? "" : " and ").append(shown(code.file));
  return text;
}

// Only a file whose rows changed, so a line leaves one alike as it was; put back as it
// was read, for a line refused after it wrote.
void save(VP::FilePort &files, const Code &code) {
  if (code.text() != code.read)
    files.save(code.file.string(), code.text());
}

void put_back(VP::FilePort &files, const Code &code) {
  if (code.text() != code.read)
    files.save(code.file.string(), code.read);
}

// The node of the view whose folder holds the file (RV08), root at its top; refused for a
// file no node of the view holds, which no connection of it reaches.
std::string node_holding(const VP::View &view, const std::filesystem::path &file) {
  if (std::optional<std::string> node = node_named(view, file))
    return std::move(*node);
  throw std::runtime_error(
      std::format("{} is in no node's folder of view {}, which the line goes to",
                  shown(file),
                  view.name));
}

std::string indent_of(std::string_view row) {
  return std::string(row.substr(0, std::min(row.find_first_not_of(" \t"), row.size())));
}

// A row's code without the comment after it, as a // in a string is none; empty for a
// row commented out.
std::string_view code_of(std::string_view row) {
  bool quoted = false;
  for (std::size_t at = 0; at + 1 < row.size(); ++at)
    if (row[at] == '"' && (at == 0 || row[at - 1] != '\\'))
      quoted = !quoted;
    else if (!quoted && row[at] == '/' && row[at + 1] == '/')
      return trim(row.substr(0, at));
  return trim(row);
}

// The braces a row's code opens, less those it closes, those in a string left out.
std::ptrdiff_t braces(std::string_view code) {
  std::ptrdiff_t depth = 0;
  bool quoted = false;
  for (std::size_t at = 0; at < code.size(); ++at)
    if (code[at] == '"' && (at == 0 || code[at - 1] != '\\'))
      quoted = !quoted;
    else if (!quoted)
      depth += code[at] == '{' ? 1 : code[at] == '}' ? -1 : 0;
  return depth;
}

// A body from a row on: the row its { opens on and the row its } closes on; none where a
// ; ends the statement first, as a function only declared, or nothing closes it.
struct Body {
  std::size_t open = 0;
  std::size_t close = 0;
};

std::optional<Body> body_from(const Rows &rows, std::size_t from) {
  std::optional<std::size_t> open;
  std::size_t depth = 0;
  for (std::size_t at = from; at < rows.size(); ++at) {
    const std::string_view code = code_of(rows[at]);
    bool quoted = false;
    for (std::size_t each = 0; each < code.size(); ++each)
      if (code[each] == '"' && (each == 0 || code[each - 1] != '\\'))
        quoted = !quoted;
      else if (quoted)
        continue;
      else if (code[each] == ';' && !open)
        return std::nullopt;
      else if (code[each] == '{' && ++depth == 1)
        open = open.value_or(at);
      else if (code[each] == '}' && open && --depth == 0)
        return Body{*open, at};
  }
  return std::nullopt;
}

// The rows of a body at its own depth, not in a function or a block inside it.
std::vector<std::size_t> top_rows(const Rows &rows, const Body &body) {
  std::vector<std::size_t> found;
  std::ptrdiff_t depth = 0;
  for (std::size_t at = body.open + 1; at < body.close; ++at) {
    if (depth == 0)
      found.push_back(at);
    depth += braces(code_of(rows[at]));
  }
  return found;
}

// The member a class's row declares, as `float _speed = 0;` or `VP::Value<float>
// _phase;`: the name before its initializer; none for a row declaring a function, or no
// name.
std::optional<std::string> member_of(std::string_view code) {
  const std::size_t initial = std::min(code.find_first_of("={["), code.size());
  if (!code.ends_with(';') || code.find('(') < initial)
    return std::nullopt;
  const std::string_view declared =
      trim(code.substr(0, std::min(initial, code.size() - 1)));
  const std::size_t before = declared.find_last_of(" *&");
  if (before == std::string_view::npos || !glsl_name(declared.substr(before + 1)))
    return std::nullopt;
  return std::string(declared.substr(before + 1));
}

// A start's row in bind() commented out, as `// _speed = node.param<float>("speed");`:
// the way it binds, param, and the member it assigns, _speed; none for any other row.
std::optional<std::pair<std::string, std::string>> hint_of(std::string_view row,
                                                           std::string_view node) {
  const std::optional<std::string> hint = uncommented(row);
  if (!hint)
    return std::nullopt;
  const std::string_view code = code_of(*hint);
  const std::size_t assigns = code.find(" = ");
  const std::string_view member = code.substr(0, std::min(assigns, code.size()));
  std::string_view call = assigns == std::string_view::npos
                              ? std::string_view()
                              : trim(code.substr(assigns + 3));
  if (call.starts_with('&'))
    call.remove_prefix(1);
  const std::string prefix = std::format("{}.", node);
  const std::size_t angle = call.find('<');
  if (!member.starts_with('_') || !glsl_name(member) || !call.starts_with(prefix) ||
      angle == std::string_view::npos)
    return std::nullopt;
  return std::pair(std::string(call.substr(prefix.size(), angle - prefix.size())),
                   std::string(member));
}

// The name a bind()'s row gives the VP::Bind it takes, as `bind(VP::Bind &node)` gives
// node; empty for none.
std::string bind_word(std::string_view code) {
  constexpr std::string_view taken = "Bind &";
  const std::size_t at = code.find(taken);
  std::string word;
  for (std::size_t each = at == std::string_view::npos ? code.size() : at + taken.size();
       each < code.size() &&
       (glsl_name(code.substr(each, 1)) || (code[each] >= '0' && code[each] <= '9'));
       ++each)
    word.push_back(code[each]);
  return word;
}

bool binds(std::string_view code) {
  return code.find(" bind(") != std::string_view::npos &&
         code.find("Bind &") != std::string_view::npos;
}

// Whether a row opens the class of that name, as `class Name final : public ...`.
bool opens_class(std::string_view code, std::string_view name) {
  for (const std::string_view key : {"class ", "struct "})
    if (code.starts_with(key) && code.substr(key.size()).starts_with(name)) {
      const std::string_view rest = code.substr(key.size() + name.size());
      return rest.empty() || rest.front() == ' ' || rest.front() == '{' ||
             rest.front() == ':';
    }
  return false;
}

// What a line binds in a node's C++, by one name: the way, as Bind names its call, the
// C++ type the call takes, the contract's header the type needs, if any, and the type as
// the line's answer says it, as the line named it or the field holds it.
struct Binding {
  std::string way;
  std::string type;
  std::string name;
  std::string include;
  std::string said;

  // The line bind() gets, as the node's VP::Bind is named there.
  std::string line(std::string_view node) const {
    const bool object = way == "output" || way == "input";
    return std::format(
        "_{} = {}{}.{}<{}>(\"{}\");", name, object ? "&" : "", node, way, type, name);
  }
  // The member the line assigns, as the start's are.
  std::string member() const {
    if (way == "param")
      return std::format("{} _{}{{}};", type, name);
    if (way == "output" || way == "input")
      return std::format(
          "{}{} *_{} = nullptr;", way == "input" ? "const " : "", type, name);
    std::string held = way;
    held.front() = static_cast<char>(held.front() - 'a' + 'A');
    return std::format("VP::{}<{}> _{};", held, type, name);
  }
};

// A change to a file's rows: rows taken out at a row, and those put there, so changes
// made from the last row up leave the rows above where they were.
struct Change {
  Code *code = nullptr;
  std::size_t at = 0;
  std::size_t taken = 0;
  Rows rows;
};

// Where a node's C++ holds its operator's class: the .cpp, or a header beside it that it
// includes, as a node's header template lays one out, the class's body there, its
// bind()'s body, in it or in the .cpp, as Class::bind defines it, and the name bind()
// gives the VP::Bind it takes.
struct Located {
  std::string name;
  Code *declared = nullptr;
  Body body;
  Code *bound = nullptr;
  Body bind;
  std::string node;
};

// The class whose rows hold it, from a row on.
std::optional<Body> class_in(const Code &code, std::string_view name) {
  for (std::size_t at = 0; at < code.rows.size(); ++at)
    if (opens_class(code_of(code.rows[at]), name))
      if (const std::optional<Body> body = body_from(code.rows, at))
        return body;
  return std::nullopt;
}

// A .cpp, then each header beside it that it includes, where its class may be.
std::vector<Code> cpp_and_headers(VP::FilePort &files, const std::filesystem::path &cpp) {
  std::vector<Code> codes{read_code(files, cpp)};
  for (const std::string &row : Rows(codes.front().rows))
    if (const std::optional<std::string> include = include_of(row))
      if (const std::filesystem::path beside = cpp.parent_path() / *include;
          include->ends_with(header_extension) && listed_file(files, beside))
        codes.push_back(read_code(files, beside));
  return codes;
}

// The class a .cpp's VP_OPERATOR(Name) names; refused for a .cpp with none (A02).
std::string operator_named(const Code &cpp) {
  const auto named = std::ranges::find_if(cpp.rows, [](const std::string &row) {
    return code_of(row).starts_with(operator_macro);
  });
  if (named == cpp.rows.end())
    throw std::runtime_error(
        std::format("{} registers no operator by VP_OPERATOR(<class>), whose class a "
                    "line declares in",
                    shown(cpp.file)));
  const std::string_view macro = code_of(*named);
  return std::string(
      trim(macro.substr(operator_macro.size(), macro.find(')') - operator_macro.size())));
}

// The class's bind() with a body, in the class or after it in the .cpp, as Class::bind
// defines it, and the name it gives the VP::Bind it takes; refused for none (A02).
void find_bind(Located &found, Code &cpp) {
  for (const std::size_t at : top_rows(found.declared->rows, found.body))
    if (const std::string_view code = code_of(found.declared->rows[at]); binds(code)) {
      found.node = bind_word(code);
      if (const std::optional<Body> inline_body = body_from(found.declared->rows, at)) {
        found.bound = found.declared;
        found.bind = *inline_body;
      }
    }
  for (std::size_t at = 0; !found.bound && at < cpp.rows.size(); ++at)
    if (const std::string_view code = code_of(cpp.rows[at]);
        code.find(found.name + "::bind(") != std::string_view::npos)
      if (const std::optional<Body> outside = body_from(cpp.rows, at)) {
        found.node = bind_word(code);
        found.bound = &cpp;
        found.bind = *outside;
      }
  if (!found.bound || found.node.empty())
    throw std::runtime_error(std::format(
        "{} has no bind(VP::Bind &node) with a body, where a line declares", found.name));
}

// Refused naming what is missing (A02): no VP_OPERATOR naming the class, no class of the
// name, no bind() with a body. The codes stay where they are while it is used.
Located located(std::vector<Code> &codes) {
  Located found{.name = operator_named(codes.front())};
  for (Code &code : codes)
    if (const std::optional<Body> body = class_in(code, found.name)) {
      found.declared = &code;
      found.body = *body;
      break;
    }
  if (!found.declared)
    throw std::runtime_error(std::format("{} names {}, which no class of it or of a "
                                         "header beside it that it includes is",
                                         shown(codes.front().file),
                                         found.name));
  find_bind(found, codes.front());
  return found;
}

// Where bind() gets a line no start's row stands for: after the statement of its last
// line naming the node, so the lines that bind stay together above the start's rows
// commented out; else after its {, which a body on one row, {}, is opened past first.
Change placed(const Located &op, const std::string &line) {
  const Rows &rows = op.bound->rows;
  std::optional<std::size_t> last;
  for (const std::size_t at : top_rows(rows, op.bind))
    if (code_of(rows[at]).find(op.node + ".") != std::string_view::npos)
      last = at;
  std::ptrdiff_t depth = 0;
  for (std::size_t at = last.value_or(op.bind.close); at < op.bind.close; ++at) {
    const std::string_view code = code_of(rows[at]);
    depth += braces(code);
    if (depth <= 0 && (code.ends_with(';') || code.ends_with('}')))
      return {op.bound, at + 1, 0, {indent_of(rows[*last]) + line}};
  }
  const std::string indent = indent_of(rows[op.bind.open]);
  const std::string row = indent + std::string(body_indent) + line;
  if (op.bind.open != op.bind.close)
    return {op.bound, op.bind.open + 1, 0, {row}};
  const std::string &both = rows[op.bind.open];
  const std::size_t close = both.rfind('}');
  return {op.bound,
          op.bind.open,
          1,
          {both.substr(0, close), row, indent + both.substr(close)}};
}

// The bind() line: in place of the start's row of that way commented out, which it
// renames, else placed. The old name the start's row assigned, which its member row names
// too; empty for none.
std::string
bind_change(const Located &op, const Binding &binding, std::vector<Change> &changes) {
  const Rows &rows = op.bound->rows;
  const std::string line = binding.line(op.node);
  for (const std::size_t at : top_rows(rows, op.bind))
    if (const auto hint = hint_of(rows[at], op.node);
        hint && hint->first == binding.way) {
      changes.push_back({op.bound, at, 1, {indent_of(rows[at]) + line}});
      return hint->second;
    }
  changes.push_back(placed(op, line));
  return {};
}

// The member: in place of the start's row commented out that the bind() row taken
// assigned; else after the class's last member, or last in it. Refused for a name a
// member holds already (A02).
void member_change(const Located &op,
                   const Binding &binding,
                   const std::string &hinted,
                   std::vector<Change> &changes) {
  Rows &rows = op.declared->rows;
  std::optional<std::size_t> last;
  std::optional<std::size_t> hint;
  for (const std::size_t at : top_rows(rows, op.body)) {
    const std::optional<std::string> held = uncommented(rows[at]);
    const std::optional<std::string> member = member_of(code_of(held.value_or(rows[at])));
    if (!held && member == "_" + binding.name)
      throw std::runtime_error(
          std::format("{} holds a member _{} already", op.name, binding.name));
    if (!held && member)
      last = at;
    if (held && member && !hinted.empty() && *member == hinted && !hint)
      hint = at;
  }
  const std::size_t at = hint ? *hint : last ? *last + 1 : op.body.close;
  const std::string indent =
      hint   ? indent_of(rows[*hint])
      : last ? indent_of(rows[*last])
             : indent_of(rows[op.body.open]) + std::string(body_indent);
  changes.push_back({op.declared, at, hint ? 1U : 0U, {indent + binding.member()}});
}

// The contract's header, among the file's includes in their order, where it has none.
void include_change(const Located &op,
                    const Binding &binding,
                    std::vector<Change> &changes) {
  const Rows &rows = op.declared->rows;
  const std::string row = std::format("#include \"{}\"", binding.include);
  if (binding.include.empty() || std::ranges::any_of(rows, [&](const std::string &each) {
        return include_of(each) == binding.include;
      }))
    return;
  std::optional<std::size_t> after;
  for (std::size_t at = 0; at < rows.size(); ++at)
    if (const std::optional<std::string> include = include_of(rows[at])) {
      if (*include > binding.include) {
        changes.push_back({op.declared, at, 0, {row}});
        return;
      }
      after = at + 1;
    }
  changes.push_back({op.declared, after.value_or(0), 0, {row}});
}

// The files a line declaring a binding writes, all known before a file is written, so a
// line refused writes none (A02): the .cpp, and the header beside it its class is in. A
// start's row of the binding's way in bind() commented out is taken, renamed, and so is
// the member row it assigned; else the line goes after bind()'s last line naming the
// node, the member after the class's last, and a contract's include among the others.
// Only bind() and the class change: cook() is the person's own.
std::vector<Code> declaring_cpp(VP::FilePort &files,
                                const std::filesystem::path &cpp,
                                const Binding &binding) {
  std::vector<Code> codes = cpp_and_headers(files, cpp);
  const Located op = located(codes);
  std::vector<Change> changes;
  const std::string hinted = bind_change(op, binding, changes);
  member_change(op, binding, hinted, changes);
  include_change(op, binding, changes);
  std::ranges::sort(
      changes, [](const Change &one, const Change &other) { return one.at > other.at; });
  for (Change &change : changes) {
    Rows &rows = change.code->rows;
    const auto at = rows.begin() + static_cast<std::ptrdiff_t>(change.at);
    rows.insert(rows.erase(at, at + static_cast<std::ptrdiff_t>(change.taken)),
                change.rows.begin(),
                change.rows.end());
  }
  std::erase_if(codes, [](const Code &code) { return code.text() == code.read; });
  return codes;
}

// The C++ type a buffer's element is written or read as: a value's, or a struct's, as a
// contract declares it for C++, whose header the line includes (RV05); refused for one no
// contract declares, which C++ could not lay out as the shader does (RA03).
void type_element(VP::FilePort &files,
                  const std::filesystem::path &file,
                  std::string_view element,
                  Binding &binding) {
  if (const auto value = std::ranges::find(value_types, element, &ValueType::glsl);
      value != value_types.end()) {
    binding.type = value->cpp;
    return;
  }
  for (CppContract &contract : cpp_contracts(files, file))
    if (contract.type == element) {
      binding.type = std::format("{}{}", view_namespace, element);
      binding.include = std::move(contract.header);
      return;
    }
  throw std::runtime_error(std::format(
      "{} holds {}, which no C++ contract of its view declares: a header in contracts/ "
      "declaring struct {} member for member as its shaders do lets C++ hold it (RV05)",
      binding.name,
      element,
      element));
}

// What a type gives a name in a node's C++: a param, a value or a word, read once as it
// binds, or an output, carrying a contract's type to other C++ nodes (RV05); refused for
// any other type, naming those it may be (A02).
Binding typed_binding(VP::FilePort &files,
                      const std::filesystem::path &file,
                      std::string_view type,
                      std::string_view name) {
  check_name(name);
  Binding binding{.way = "param", .name = std::string(name), .said = std::string(type)};
  if (const auto value = std::ranges::find(value_types, type, &ValueType::glsl);
      value != value_types.end())
    binding.type = value->cpp;
  else if (type == word_type.glsl)
    binding.type = word_type.cpp;
  for (CppContract &contract :
       binding.type.empty() ? cpp_contracts(files, file) : std::vector<CppContract>())
    if (contract.type == type) {
      binding.way = "output";
      binding.type = std::format("{}{}", view_namespace, type);
      binding.include = std::move(contract.header);
    }
  if (binding.type.empty())
    throw std::runtime_error(
        std::format("{} is no type {} may declare; it may declare {}",
                    type,
                    shown(file),
                    listed(field_types(files, file))));
  return binding;
}

// What a field of its node's shaders, named alone, gives a .cpp: a value it sets each
// frame, a Texture it fills, a reading buffer it uploads, or a writing one it reads back,
// each typed as the field holds it; refused for a field it may not take, naming why.
Binding field_binding(VP::FilePort &files,
                      const VP::View &view,
                      const std::filesystem::path &file,
                      std::string_view name) {
  const std::string node = node_holding(view, file);
  const std::vector<CppField> fields = cpp_fields(files, file, view);
  const auto field = std::ranges::find_if(
      fields, [&](const CppField &each) { return each.field.name == name; });
  if (field == fields.end()) {
    const std::vector<std::string> free = free_fields(files, file, view);
    throw std::runtime_error(std::format(
        "{} is no field of the shaders of {} that C++ may fill, which are {}; a param or "
        "an output takes a type and a name: param declare {} <type> <name>",
        name,
        node,
        free.empty() ? std::string("none") : listed(free),
        shown(file)));
  }
  if (!field->taken.empty())
    throw std::runtime_error(
        std::format("{} may not fill {}: {}", shown(file), name, field->taken));
  const std::string &type = field->field.type;
  Binding binding{.name = std::string(name), .said = type};
  if (const auto value = std::ranges::find(value_types, type, &ValueType::glsl);
      value != value_types.end()) {
    binding.way = "value";
    binding.type = value->cpp;
    return binding;
  }
  if (type == VP::texture_type) {
    binding.way = "texture";
    binding.type = default_pixel;
    return binding;
  }
  const NodeFields held = node_fields(files, file.parent_path());
  for (const BufferType &buffer :
       buffers_of(files, held.shader, files.read(held.shader.string())))
    if (buffer.name == type) {
      binding.way = buffer.writes ? "readback" : "upload";
      type_element(files, file, buffer.element, binding);
      return binding;
    }
  throw std::runtime_error(
      std::format("{} is a {}, which C++ fills none of", name, type));
}

// What reads a C++ output another node writes: an input of the contract's type it carries
// (RV05); refused for any other port, naming the C++ outputs there are (A02).
Binding input_binding(VP::Call &call,
                      const std::filesystem::path &file,
                      std::string_view port,
                      std::string_view name) {
  std::vector<std::string> outputs;
  for (const VP::NodePort &each : call.ports()) {
    const std::string text = std::format("{}.{}", each.node, each.name);
    if (each.writes && text != port && each.kind == "object")
      outputs.push_back(text);
    if (!each.writes || text != port)
      continue;
    if (each.kind != "object")
      throw std::runtime_error(std::format(
          "{} is {} a shader reads, not C++: connect declare {} <shader> {} declares it",
          port,
          each.kind == "image" ? "an image" : "a buffer",
          port,
          name));
    const std::size_t scope = each.holds.rfind("::");
    const std::string type =
        each.holds.substr(scope == std::string::npos ? 0 : scope + 2);
    for (CppContract &contract : cpp_contracts(call.files(), file))
      if (contract.type == type)
        return {.way = "input",
                .type = std::format("{}{}", view_namespace, type),
                .name = std::string(name),
                .include = std::move(contract.header),
                .said = type};
    throw std::runtime_error(std::format("{} carries a {}, which no C++ contract of the "
                                         "view declares, as a C++ input reads one (RV05)",
                                         port,
                                         each.holds));
  }
  throw std::runtime_error(
      std::format("{} is no C++ output a node of view {} writes; they are {}",
                  port,
                  call.view().name,
                  outputs.empty() ? std::string("none") : listed(outputs)));
}

// What uses a binding next, which no line writes, as cook() is the person's own: the line
// setting a param, or the start's cook() line of its way.
std::string next_use(const Binding &binding, const std::optional<std::string> &node) {
  if (binding.way == "param")
    return node ? std::format("which param set {} {} <value> sets", *node, binding.name)
                : std::format("read as param {}", binding.name);
  if (binding.way == "output")
    return std::format("an output its cook writes through _{}", binding.name);
  if (binding.way == "value")
    return std::format("which its cook sets by frame.set(_{}, ...)", binding.name);
  if (binding.way == "upload")
    return std::format("which its cook fills by frame.write(_{})", binding.name);
  if (binding.way == "texture")
    return std::format("which its cook fills by frame.upload(_{}, ...)", binding.name);
  return std::format("which its cook reads by frame.read(_{}), a frame late",
                     binding.name);
}

} // namespace
