#include "baseclasses/Pipelines.h"

#include "baseclasses/Mechanics.h"

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
constexpr std::uint32_t storage_physical_storage_buffer = 5349;
} // namespace spirv

constexpr std::uint32_t scalar_bytes = 4;
constexpr std::uint32_t address_bytes = sizeof(VkDeviceAddress);
constexpr std::uint32_t std140_block_alignment = 16;
constexpr std::uint32_t max_passes = 1024;
constexpr float line_width = 1.0f; // the only width without the wideLines feature

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

// A buffer is a pointer to a block whose first member is a runtime array: the array's
// stride is the element size, and the member's qualifier says how the shader uses it.
Field describe_buffer(const Module &module, Field field, std::uint32_t block) {
  const std::vector<std::uint32_t> &array = module.types.at(module.types.at(block).at(1));
  if (array.at(0) != spirv::op_type_runtime_array)
    throw std::runtime_error(std::format(
        "buffer {} must hold one runtime array, such as float at[]", field.name));
  const Member &data = module.members.at(block).at(0);
  field.type = glsl_name(module, array.at(1));
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
  if (value_bytes(module, type_id) == 0)
    throw std::runtime_error(
        std::format("pass block field {} has a type this build cannot set yet: use "
                    "float, int, uint, a vector of them, or a buffer",
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
      fields.push_back(
          describe(module, module.members.at(block).at(index - 1), members[index]));
      end = std::max(end,
                     fields.back().offset + (fields.back().buffer()
                                                 ? address_bytes
                                                 : value_bytes(module, members[index])));
    }
    const std::uint32_t size = (end + std140_block_alignment - 1) /
                               std140_block_alignment * std140_block_alignment;
    return {std::move(fields), size};
  }
  return {};
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

// A shader module lives only until the pipeline made from it exists.
class ShaderModule {
public:
  ShaderModule(VkDevice device, const Shader &shader) : _device(device) {
    const std::span<const std::uint32_t> words = shader.words();
    const VkShaderModuleCreateInfo code{.sType =
                                            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = words.size_bytes(),
                                        .pCode = words.data()};
    check(vkCreateShaderModule(_device, &code, nullptr, &_module),
          "vkCreateShaderModule");
  }
  ~ShaderModule() {
    vkDestroyShaderModule(_device, _module, nullptr);
  }
  ShaderModule(const ShaderModule &) = delete;
  ShaderModule &operator=(const ShaderModule &) = delete;

  VkShaderModule handle() const {
    return _module;
  }

private:
  const VkDevice _device;
  VkShaderModule _module = VK_NULL_HANDLE;
};

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
  std::tie(_fields, _block_size) = pass_block(module);
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

const std::array<std::uint32_t, 3> &Shader::workgroup_size() const {
  return _workgroup_size;
}

Pipelines::Pipelines(VkDevice device, const Resources &resources)
    : _device(device), _resources(resources) {
  const VkDescriptorSetLayoutCreateInfo images{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  check(vkCreateDescriptorSetLayout(_device, &images, nullptr, &_images),
        "vkCreateDescriptorSetLayout");
  const VkDescriptorSetLayoutBinding block{.binding = pass_binding,
                                           .descriptorType =
                                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .descriptorCount = 1,
                                           .stageFlags = VK_SHADER_STAGE_ALL};
  const VkDescriptorSetLayoutCreateInfo pass{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &block};
  check(vkCreateDescriptorSetLayout(_device, &pass, nullptr, &_pass),
        "vkCreateDescriptorSetLayout");
  const std::array sets{_images, _pass}; // in set order
  const VkPipelineLayoutCreateInfo layout{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = static_cast<std::uint32_t>(sets.size()),
      .pSetLayouts = sets.data()};
  check(vkCreatePipelineLayout(_device, &layout, nullptr, &_layout),
        "vkCreatePipelineLayout");
  const VkDescriptorPoolSize blocks{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                    .descriptorCount = max_passes};
  const VkDescriptorPoolCreateInfo pool{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = max_passes,
      .poolSizeCount = 1,
      .pPoolSizes = &blocks};
  check(vkCreateDescriptorPool(_device, &pool, nullptr, &_pool),
        "vkCreateDescriptorPool");
}

Pipelines::~Pipelines() {
  vkDestroyDescriptorPool(_device, _pool, nullptr);
  vkDestroyPipelineLayout(_device, _layout, nullptr);
  vkDestroyDescriptorSetLayout(_device, _pass, nullptr);
  vkDestroyDescriptorSetLayout(_device, _images, nullptr);
}

VkPipelineLayout Pipelines::layout() const {
  return _layout;
}

Pipeline::Pipeline(const Pipelines &pipelines, const Shader &compute)
    : _device(pipelines._device) {
  const ShaderModule module(_device, compute);
  const VkComputePipelineCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module.handle(),
                .pName = "main"},
      .layout = pipelines.layout()};
  check(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &info, nullptr, &_pipeline),
        "vkCreateComputePipelines");
}

// The vertex shader pulls its vertices from buffers by address (RV02), so the pipeline
// takes no vertex input; the viewport follows the window, so it is set per frame.
Pipeline::Pipeline(const Pipelines &pipelines,
                   const Shader &vertex,
                   const Shader &fragment,
                   VkRenderPass render_pass)
    : _device(pipelines._device) {
  const ShaderModule vertex_module(_device, vertex);
  const ShaderModule fragment_module(_device, fragment);
  const std::array stages{
      VkPipelineShaderStageCreateInfo{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vertex_module.handle(),
          .pName = "main"},
      VkPipelineShaderStageCreateInfo{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fragment_module.handle(),
          .pName = "main"}};
  const VkPipelineVertexInputStateCreateInfo input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  const VkPipelineInputAssemblyStateCreateInfo assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  const VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1};
  const VkPipelineRasterizationStateCreateInfo rasterization{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = line_width};
  const VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  const VkPipelineColorBlendAttachmentState color{
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
  const VkPipelineColorBlendStateCreateInfo blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &color};
  const std::array dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  const VkPipelineDynamicStateCreateInfo dynamic_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<std::uint32_t>(dynamic.size()),
      .pDynamicStates = dynamic.data()};
  const VkGraphicsPipelineCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = static_cast<std::uint32_t>(stages.size()),
      .pStages = stages.data(),
      .pVertexInputState = &input,
      .pInputAssemblyState = &assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic_state,
      .layout = pipelines.layout(),
      .renderPass = render_pass};
  check(
      vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &info, nullptr, &_pipeline),
      "vkCreateGraphicsPipelines");
}

Pipeline::Pipeline(Pipeline &&other) noexcept
    : _device(other._device), _pipeline(std::exchange(other._pipeline, VK_NULL_HANDLE)) {}

Pipeline &Pipeline::operator=(Pipeline &&other) noexcept {
  std::swap(_device, other._device);
  std::swap(_pipeline, other._pipeline);
  return *this;
}

Pipeline::~Pipeline() {
  if (_pipeline)
    vkDestroyPipeline(_device, _pipeline, nullptr);
}

VkPipeline Pipeline::handle() const {
  return _pipeline;
}

PassBlock::PassBlock(const Pipelines &pipelines, std::uint32_t size)
    : _pipelines(&pipelines),
      _buffer(pipelines._resources.buffer(
          size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Memory::upload)) {
  const VkDescriptorSetAllocateInfo allocate{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pipelines._pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &pipelines._pass};
  check(vkAllocateDescriptorSets(pipelines._device, &allocate, &_set),
        "vkAllocateDescriptorSets");
  const VkDescriptorBufferInfo buffer{.buffer = _buffer.handle(), .range = VK_WHOLE_SIZE};
  const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = _set,
                                   .dstBinding = pass_binding,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                   .pBufferInfo = &buffer};
  vkUpdateDescriptorSets(pipelines._device, 1, &write, 0, nullptr);
}

PassBlock::PassBlock(PassBlock &&other) noexcept
    : _pipelines(other._pipelines), _buffer(std::move(other._buffer)),
      _set(std::exchange(other._set, VK_NULL_HANDLE)) {}

PassBlock &PassBlock::operator=(PassBlock &&other) noexcept {
  std::swap(_pipelines, other._pipelines);
  std::swap(_buffer, other._buffer);
  std::swap(_set, other._set);
  return *this;
}

PassBlock::~PassBlock() {
  if (_set)
    vkFreeDescriptorSets(_pipelines->_device, _pipelines->_pool, 1, &_set);
}

VkDescriptorSet PassBlock::set() const {
  return _set;
}

std::span<std::byte> PassBlock::bytes() const {
  return _buffer.bytes();
}

void PassBlock::flush() const {
  _buffer.flush();
}

} // namespace VP
