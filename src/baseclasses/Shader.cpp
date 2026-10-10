#include "baseclasses/Shader.h"

#include <algorithm>
#include <climits>
#include <format>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace VP {

namespace {

// The numbers reflection reads, as the SPIR-V specification defines them.
namespace spirv {
constexpr std::uint32_t magic = 0x07230203;
constexpr std::size_t header_words = 5;
constexpr std::uint32_t word_count_shift = 16;
constexpr std::uint32_t opcode_mask = 0xffff;
constexpr std::uint32_t op_name = 5;
constexpr std::uint32_t op_member_name = 6;
constexpr std::uint32_t op_execution_mode = 16;
constexpr std::uint32_t op_type_int = 21;
constexpr std::uint32_t op_type_float = 22;
constexpr std::uint32_t op_type_vector = 23;
constexpr std::uint32_t op_type_runtime_array = 29;
constexpr std::uint32_t op_type_struct = 30;
constexpr std::uint32_t op_type_pointer = 32;
constexpr std::uint32_t op_variable = 59;
constexpr std::uint32_t op_decorate = 71;
constexpr std::uint32_t op_member_decorate = 72;
constexpr std::uint32_t mode_local_size = 17;
constexpr std::uint32_t decoration_array_stride = 6;
constexpr std::uint32_t decoration_non_writable = 24;
constexpr std::uint32_t decoration_non_readable = 25;
constexpr std::uint32_t decoration_binding = 33;
constexpr std::uint32_t decoration_descriptor_set = 34;
constexpr std::uint32_t decoration_offset = 35;
constexpr std::uint32_t storage_uniform = 2;
constexpr std::uint32_t storage_output = 3;
constexpr std::uint32_t storage_push_constant = 9;
constexpr std::uint32_t storage_physical_storage_buffer = 5349;
} // namespace spirv

constexpr std::uint32_t scalar_bytes = 4;
// A buffer is a 64-bit device address in the pass block (RV02).
constexpr std::uint32_t address_bytes = sizeof(std::uint64_t);
constexpr std::uint32_t std140_block_alignment = 16;
// How GLSL starts the names of its built-ins, which are no ports.
constexpr std::string_view built_in = "gl_";

struct Member {
  std::string name;
  std::uint32_t offset = 0;
  bool non_writable = false;
  bool non_readable = false;
};

// What one pass over the words collects, by result id. A type keeps its opcode first,
// then the operands after its result id.
struct Module {
  std::unordered_map<std::uint32_t, std::string> names;
  std::unordered_map<std::uint32_t, std::vector<Member>> members;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> types;
  std::unordered_map<std::uint32_t, std::uint32_t> strides, sets, bindings;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> uniforms; // pointer type, variable
  std::vector<std::uint32_t> push_constants;                     // pointer types
  std::vector<std::uint32_t> outputs;                            // variables
  std::array<std::uint32_t, 3> workgroup_size{};

  Member &member(std::uint32_t type, std::uint32_t index) {
    std::vector<Member> &list = members[type];
    if (list.size() <= index)
      list.resize(index + 1);
    return list[index];
  }
};

std::uint32_t at(std::span<const std::uint32_t> operands, std::size_t index) {
  if (index >= operands.size())
    throw std::runtime_error("SPIR-V instruction is shorter than its opcode needs");
  return operands[index];
}

// UTF-8, four bytes to a word, low byte first, ending in a nul.
std::string literal(std::span<const std::uint32_t> words) {
  std::string text;
  for (const std::uint32_t word : words)
    for (std::size_t byte = 0; byte < sizeof word; ++byte) {
      const char character = static_cast<char>(word >> (CHAR_BIT * byte));
      if (character == '\0')
        return text;
      text += character;
    }
  return text;
}

void decorate(Module &module, std::span<const std::uint32_t> operands) {
  const std::uint32_t target = at(operands, 0);
  switch (at(operands, 1)) {
    case spirv::decoration_array_stride:
      module.strides[target] = at(operands, 2);
      break;
    case spirv::decoration_descriptor_set:
      module.sets[target] = at(operands, 2);
      break;
    case spirv::decoration_binding:
      module.bindings[target] = at(operands, 2);
      break;
  }
}

void decorate_member(Module &module, std::span<const std::uint32_t> operands) {
  Member &member = module.member(at(operands, 0), at(operands, 1));
  switch (at(operands, 2)) {
    case spirv::decoration_offset:
      member.offset = at(operands, 3);
      break;
    case spirv::decoration_non_writable:
      member.non_writable = true;
      break;
    case spirv::decoration_non_readable:
      member.non_readable = true;
      break;
  }
}

void read(Module &module, std::uint32_t opcode, std::span<const std::uint32_t> operands) {
  switch (opcode) {
    case spirv::op_name: {
      const std::uint32_t id = at(operands, 0);
      module.names[id] = literal(operands.subspan(1));
      break;
    }
    case spirv::op_member_name: {
      Member &member = module.member(at(operands, 0), at(operands, 1));
      member.name = literal(operands.subspan(2));
      break;
    }
    case spirv::op_execution_mode:
      if (at(operands, 1) == spirv::mode_local_size)
        module.workgroup_size = {at(operands, 2), at(operands, 3), at(operands, 4)};
      break;
    case spirv::op_type_int:
    case spirv::op_type_float:
    case spirv::op_type_vector:
    case spirv::op_type_runtime_array:
    case spirv::op_type_struct:
    case spirv::op_type_pointer: {
      std::vector<std::uint32_t> &type = module.types[at(operands, 0)];
      type.assign(1, opcode);
      type.insert(type.end(), operands.begin() + 1, operands.end());
      break;
    }
    case spirv::op_variable:
      if (at(operands, 2) == spirv::storage_uniform)
        module.uniforms.emplace_back(at(operands, 0), at(operands, 1));
      else if (at(operands, 2) == spirv::storage_push_constant)
        module.push_constants.push_back(at(operands, 0));
      else if (at(operands, 2) == spirv::storage_output)
        module.outputs.push_back(at(operands, 1));
      break;
    case spirv::op_decorate:
      decorate(module, operands);
      break;
    case spirv::op_member_decorate:
      decorate_member(module, operands);
      break;
  }
}

// A type as GLSL names it, so C++ and the other end of a connection can be compared
// with it; empty for a type reflection does not name yet.
std::string glsl_name(const Module &module, std::uint32_t type_id) {
  const auto found = module.types.find(type_id);
  if (found == module.types.end())
    return {};
  const std::vector<std::uint32_t> &type = found->second;
  const bool scalar = type.size() > 1 && type[1] == CHAR_BIT * scalar_bytes;
  if (type[0] == spirv::op_type_float && scalar)
    return "float";
  if (type[0] == spirv::op_type_int && scalar)
    return type.at(2) ? "int" : "uint";
  if (type[0] == spirv::op_type_vector) {
    const std::string component = glsl_name(module, type.at(1));
    const std::string prefix = component == "float" ? "" : component.substr(0, 1);
    return component.empty() ? "" : std::format("{}vec{}", prefix, type.at(2));
  }
  if (type[0] == spirv::op_type_struct && module.names.contains(type_id))
    return module.names.at(type_id);
  return {};
}

// Bytes of a value C++ can set: a 32-bit scalar or a vector of them; 0 for any other.
std::uint32_t value_bytes(const Module &module, std::uint32_t type_id) {
  const auto found = module.types.find(type_id);
  if (found == module.types.end())
    return 0;
  const std::vector<std::uint32_t> &type = found->second;
  if (type[0] == spirv::op_type_vector)
    return type.at(2) * value_bytes(module, type.at(1));
  const bool scalar = type[0] == spirv::op_type_int || type[0] == spirv::op_type_float;
  return scalar && type.at(1) == CHAR_BIT * scalar_bytes ? scalar_bytes : 0;
}

// A struct's members as GLSL lays them out; none for any other type.
std::vector<Field> members_of(const Module &module, std::uint32_t type_id) {
  const std::vector<std::uint32_t> &type = module.types.at(type_id);
  std::vector<Field> members;
  if (type[0] != spirv::op_type_struct)
    return members;
  for (std::size_t index = 1; index < type.size(); ++index) {
    const Member &member = module.members.at(type_id).at(index - 1);
    members.push_back({.name = member.name,
                       .type = glsl_name(module, type[index]),
                       .offset = member.offset});
  }
  return members;
}

// A buffer is a pointer to a block whose first member is a runtime array: the array's
// stride is the element size, and the member's qualifier says how the shader uses it.
Field describe_buffer(const Module &module, Field field, std::uint32_t block) {
  const std::vector<std::uint32_t> &array = module.types.at(module.types.at(block).at(1));
  if (array.at(0) != spirv::op_type_runtime_array)
    throw std::runtime_error(std::format(
        "buffer {} must hold one runtime array, such as float at[]", field.name));
  const Member &data = module.members.at(block).at(0);
  field.type = glsl_name(module, array.at(1));
  field.members = members_of(module, array.at(1));
  field.stride = module.strides.at(module.types.at(block).at(1));
  field.access = data.non_writable   ? Access::read
                 : data.non_readable ? Access::write
                                     : Access::read_write;
  return field;
}

Field describe(const Module &module, const Member &member, std::uint32_t type_id) {
  const std::vector<std::uint32_t> &type = module.types.at(type_id);
  if (type[0] == spirv::op_type_pointer &&
      type.at(1) == spirv::storage_physical_storage_buffer)
    return describe_buffer(
        module, {.name = member.name, .offset = member.offset}, type.at(2));
  if (glsl_name(module, type_id) == texture_type)
    return {
        .name = member.name, .type = std::string(texture_type), .offset = member.offset};
  if (value_bytes(module, type_id) == 0)
    throw std::runtime_error(
        std::format("pass block field {} has a type this build cannot set yet: use "
                    "float, int, uint, a vector of them, a buffer or a Texture",
                    member.name));
  return {
      .name = member.name, .type = glsl_name(module, type_id), .offset = member.offset};
}

// The pass block's fields, and its size rounded up to a whole std140 block.
std::pair<std::vector<Field>, std::uint32_t> pass_block(const Module &module) {
  for (const auto &[pointer, variable] : module.uniforms) {
    const auto set = module.sets.find(variable);
    const auto binding = module.bindings.find(variable);
    if (set == module.sets.end() || set->second != pass_set ||
        binding == module.bindings.end() || binding->second != pass_binding)
      continue;
    const std::uint32_t block = module.types.at(pointer).at(2);
    const std::vector<std::uint32_t> &members = module.types.at(block);
    std::vector<Field> fields;
    std::uint32_t end = 0;
    for (std::size_t index = 1; index < members.size(); ++index) {
      const Field &field = fields.emplace_back(
          describe(module, module.members.at(block).at(index - 1), members[index]));
      // A Texture holds its slot as a uint.
      const std::uint32_t bytes = field.buffer()    ? address_bytes
                                  : field.texture() ? scalar_bytes
                                                    : value_bytes(module, members[index]);
      end = std::max(end, field.offset + bytes);
    }
    const std::uint32_t size = (end + std140_block_alignment - 1) /
                               std140_block_alignment * std140_block_alignment;
    return {std::move(fields), size};
  }
  return {};
}

// The block the push constant's one member points to, described as the pass block's
// fields are, and its size. A shader has no other push constant, since every pass pushes
// this one address (RV02).
std::pair<std::vector<Field>, std::uint32_t> frame_block(const Module &module) {
  if (module.push_constants.empty())
    return {};
  const std::uint32_t push = module.types.at(module.push_constants.front()).at(2);
  const std::vector<std::uint32_t> &members = module.types.at(push);
  const auto address =
      members.size() == 2 ? module.types.find(members[1]) : module.types.end();
  if (module.push_constants.size() != 1 || address == module.types.end() ||
      address->second.at(0) != spirv::op_type_pointer ||
      address->second.at(1) != spirv::storage_physical_storage_buffer)
    throw std::runtime_error("its push constant is not the frame block's address, as "
                             "baseclasses/GpuLayout.glsl declares it (RV02)");
  const std::uint32_t block = address->second.at(2);
  const std::vector<std::uint32_t> &block_members = module.types.at(block);
  std::vector<Field> fields;
  std::uint32_t end = 0;
  for (std::size_t index = 1; index < block_members.size(); ++index) {
    fields.push_back(
        describe(module, module.members.at(block).at(index - 1), block_members[index]));
    end = std::max(end, fields.back().offset + value_bytes(module, block_members[index]));
  }
  return {std::move(fields), end};
}

// What the shader writes out of its stage, by name; a built-in, or a block GLSL leaves
// unnamed, is none.
std::vector<std::string> outputs_of(const Module &module) {
  std::vector<std::string> outputs;
  for (const std::uint32_t output : module.outputs) {
    const auto name = module.names.find(output);
    if (name != module.names.end() && !name->second.empty() &&
        !name->second.starts_with(built_in))
      outputs.push_back(name->second);
  }
  return outputs;
}

std::vector<std::uint32_t> read_words(const std::filesystem::path &spirv) {
  std::ifstream file(spirv, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::runtime_error(std::format("{} cannot be read", spirv.string()));
  std::vector<std::uint32_t> words(static_cast<std::size_t>(file.tellg()) /
                                   sizeof(std::uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(words.data()),
            static_cast<std::streamsize>(words.size() * sizeof(std::uint32_t)));
  return words;
}

} // namespace

Shader::Shader(const std::filesystem::path &spirv) : _words(read_words(spirv)) {
  if (_words.size() < spirv::header_words || _words[0] != spirv::magic)
    throw std::runtime_error(std::format("{} is not SPIR-V", spirv.string()));
  Module module;
  for (std::size_t word = spirv::header_words; word < _words.size();) {
    const std::uint32_t count = _words[word] >> spirv::word_count_shift;
    if (count == 0 || word + count > _words.size())
      throw std::runtime_error(std::format("{} is truncated", spirv.string()));
    read(module,
         _words[word] & spirv::opcode_mask,
         std::span<const std::uint32_t>(_words).subspan(word + 1, count - 1));
    word += count;
  }
  _workgroup_size = module.workgroup_size;
  _outputs = outputs_of(module);
  std::tie(_fields, _block_size) = pass_block(module);
  std::tie(_frame, _frame_size) = frame_block(module);
}

std::span<const std::uint32_t> Shader::words() const {
  return _words;
}

const std::vector<Field> &Shader::fields() const {
  return _fields;
}

std::uint32_t Shader::block_size() const {
  return _block_size;
}

const std::vector<Field> &Shader::frame() const {
  return _frame;
}

std::uint32_t Shader::frame_size() const {
  return _frame_size;
}

const std::array<std::uint32_t, 3> &Shader::workgroup_size() const {
  return _workgroup_size;
}

const std::vector<std::string> &Shader::outputs() const {
  return _outputs;
}

} // namespace VP
