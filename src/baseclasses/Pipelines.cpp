#include "baseclasses/Pipelines.h"

#include "baseclasses/Mechanics.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace VP {

namespace {

constexpr std::uint32_t blocks_per_pool = 1024;
// What the engine writes into the frame block, in its order, by the names and types
// baseclasses/GpuLayout.glsl gives them.
enum FrameMember : std::size_t { resolution, cursor, time, frame_index };
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> frame_members{
    {{"resolution", "uvec2"}, {"cursor", "vec2"}, {"time", "float"}, {"index", "uint"}}};
constexpr float line_width = 1.0f; // the only width without the wideLines feature
constexpr std::uint32_t textures_binding = 0;
constexpr std::uint32_t samplers_binding = 1;
// textures[]'s length, for every view at once; slot 0 means unbound.
constexpr std::uint32_t image_slots = 1024;
// The static samplers, in the order baseclasses/GpuLayout.glsl names them.
constexpr std::array<std::pair<VkFilter, VkSamplerAddressMode>, static_samplers> samplers{
    {{VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
     {VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
     {VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT},
     {VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT}}};

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

VkSampler make_sampler(VkDevice device, VkFilter filter, VkSamplerAddressMode address) {
  const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                 .magFilter = filter,
                                 .minFilter = filter,
                                 .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                 .addressModeU = address,
                                 .addressModeV = address,
                                 .addressModeW = address};
  VkSampler sampler = VK_NULL_HANDLE;
  check(vkCreateSampler(device, &info, nullptr, &sampler), "vkCreateSampler");
  return sampler;
}

// Set 0: a slot for every image a shader samples, written as images come and go, and
// the static samplers, which never change. Update after bind raises the device's limit
// on sampled images to what descriptor indexing guarantees (RV01).
VkDescriptorSetLayout image_layout(VkDevice device, std::span<const VkSampler> samplers) {
  const std::array<VkDescriptorBindingFlags, 2> flags{
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
      0};
  const VkDescriptorSetLayoutBindingFlagsCreateInfo bound{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
      .bindingCount = static_cast<std::uint32_t>(flags.size()),
      .pBindingFlags = flags.data()};
  const std::array bindings{
      VkDescriptorSetLayoutBinding{.binding = textures_binding,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   .descriptorCount = image_slots,
                                   .stageFlags = VK_SHADER_STAGE_ALL},
      VkDescriptorSetLayoutBinding{.binding = samplers_binding,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                   .descriptorCount =
                                       static_cast<std::uint32_t>(samplers.size()),
                                   .stageFlags = VK_SHADER_STAGE_ALL,
                                   .pImmutableSamplers = samplers.data()}};
  const VkDescriptorSetLayoutCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = &bound,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
      .bindingCount = static_cast<std::uint32_t>(bindings.size()),
      .pBindings = bindings.data()};
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  check(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout),
        "vkCreateDescriptorSetLayout");
  return layout;
}

// The one set 0, from a pool of its own, since update after bind needs one.
VkDescriptorPool image_pool(VkDevice device) {
  const std::array sizes{VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                              .descriptorCount = image_slots},
                         VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLER,
                                              .descriptorCount = static_samplers}};
  const VkDescriptorPoolCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
      .maxSets = 1,
      .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
      .pPoolSizes = sizes.data()};
  VkDescriptorPool pool = VK_NULL_HANDLE;
  check(vkCreateDescriptorPool(device, &info, nullptr, &pool), "vkCreateDescriptorPool");
  return pool;
}

// A draw's image starts each frame cleared, so what it held before is dropped. Once
// drawn, every shader stage that samples it after its render pass sees its pixels, in
// the layout sampling takes; earlier frames are behind the fence already.
VkRenderPass make_render_pass(VkDevice device, VkFormat format) {
  const VkAttachmentDescription color{.format = format,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                      .finalLayout =
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkAttachmentReference target{.attachment = 0,
                                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentCount = 1,
                                     .pColorAttachments = &target};
  const std::array dependencies{
      VkSubpassDependency{.srcSubpass = VK_SUBPASS_EXTERNAL,
                          .dstSubpass = 0,
                          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
      VkSubpassDependency{.srcSubpass = 0,
                          .dstSubpass = VK_SUBPASS_EXTERNAL,
                          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
  const VkRenderPassCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &color,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = static_cast<std::uint32_t>(dependencies.size()),
      .pDependencies = dependencies.data()};
  VkRenderPass render_pass = VK_NULL_HANDLE;
  check(vkCreateRenderPass(device, &info, nullptr, &render_pass), "vkCreateRenderPass");
  return render_pass;
}

} // namespace

Pipelines::Pipelines(VkDevice device, const Resources &resources)
    : _device(device), _resources(resources) {
  for (std::size_t index = 0; index < _samplers.size(); ++index)
    _samplers[index] =
        make_sampler(_device, samplers[index].first, samplers[index].second);
  _images = image_layout(_device, _samplers);
  _image_pool = image_pool(_device);
  const VkDescriptorSetAllocateInfo images{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = _image_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &_images};
  check(vkAllocateDescriptorSets(_device, &images, &_image_set),
        "vkAllocateDescriptorSets");
  for (std::uint32_t slot = image_slots - 1; slot > 0; --slot)
    _free.push_back(slot);
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
  // The frame block's address, which every pass pushes (RV02).
  const VkPushConstantRange frame{.stageFlags = VK_SHADER_STAGE_ALL,
                                  .size = sizeof(VkDeviceAddress)};
  const VkPipelineLayoutCreateInfo layout{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = static_cast<std::uint32_t>(sets.size()),
      .pSetLayouts = sets.data(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &frame};
  check(vkCreatePipelineLayout(_device, &layout, nullptr, &_layout),
        "vkCreatePipelineLayout");
}

Pipelines::~Pipelines() {
  for (const auto &[format, render_pass] : _render_passes)
    vkDestroyRenderPass(_device, render_pass, nullptr);
  for (const Pool &pool : _pools)
    vkDestroyDescriptorPool(_device, pool.handle, nullptr);
  vkDestroyPipelineLayout(_device, _layout, nullptr);
  vkDestroyDescriptorSetLayout(_device, _pass, nullptr);
  vkDestroyDescriptorPool(_device, _image_pool, nullptr);
  vkDestroyDescriptorSetLayout(_device, _images, nullptr);
  for (const VkSampler sampler : _samplers)
    vkDestroySampler(_device, sampler, nullptr);
}

VkPipelineLayout Pipelines::layout() const {
  return _layout;
}

VkDescriptorSet Pipelines::images() const {
  return _image_set;
}

VkDeviceAddress Pipelines::frame() const {
  return _frame ? _frame->buffer.address() : 0;
}

VkRenderPass Pipelines::render_pass(VkFormat format) const {
  const auto made = std::ranges::find(
      _render_passes, format, &std::pair<VkFormat, VkRenderPass>::first);
  if (made != _render_passes.end())
    return made->second;
  return _render_passes.emplace_back(format, make_render_pass(_device, format)).second;
}

// The GPU of the last frame is done with it: one frame is in flight.
void Pipelines::write_frame(std::uint64_t index,
                            double time,
                            VkExtent2D resolution,
                            std::array<float, 2> cursor) const {
  if (!_frame)
    return;
  const std::array<std::uint32_t, 2> size{resolution.width, resolution.height};
  const auto seconds = static_cast<float>(time);
  const auto frame = static_cast<std::uint32_t>(index);
  std::byte *const bytes = _frame->buffer.bytes().data();
  const std::vector<Field> &fields = _frame->fields;
  std::memcpy(bytes + fields[FrameMember::resolution].offset, size.data(), sizeof size);
  std::memcpy(bytes + fields[FrameMember::cursor].offset, cursor.data(), sizeof cursor);
  std::memcpy(bytes + fields[FrameMember::time].offset, &seconds, sizeof seconds);
  std::memcpy(bytes + fields[FrameMember::frame_index].offset, &frame, sizeof frame);
  _frame->buffer.flush();
}

// Every shader includes the same declaration, so the first one's layout is every one's.
void Pipelines::take_frame(const Shader &shader) const {
  const std::vector<Field> &fields = shader.frame();
  if (fields.empty())
    return;
  if (_frame) {
    if (fields != _frame->fields)
      throw std::runtime_error("its frame block is laid out unlike the other shaders'; "
                               "include baseclasses/GpuLayout.glsl as they do");
    return;
  }
  if (!std::ranges::equal(
          fields, frame_members, [](const Field &field, const auto &member) {
            return field.name == member.first && field.type == member.second;
          }))
    throw std::runtime_error("its frame block is not the one the engine writes: uvec2 "
                             "resolution, vec2 cursor, float time and uint index, as "
                             "baseclasses/GpuLayout.glsl declares them (RV02)");
  Buffer buffer = _resources.buffer(
      shader.frame_size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Memory::upload);
  std::ranges::fill(buffer.bytes(), std::byte{});
  buffer.flush();
  _frame.emplace(Frame{std::move(buffer), fields});
}

// Counted, not left to the pool: past maxSets one driver fails and another does not
// (C01). Every set is one block's descriptor, which Vulkan guarantees never fragments a
// pool, so a count below maxSets always allocates.
std::size_t Pipelines::pool() const {
  const auto room = std::ranges::find_if(
      _pools, [](const Pool &pool) { return pool.blocks < blocks_per_pool; });
  if (room != _pools.end())
    return static_cast<std::size_t>(room - _pools.begin());
  const VkDescriptorPoolSize blocks{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                    .descriptorCount = blocks_per_pool};
  const VkDescriptorPoolCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = blocks_per_pool,
      .poolSizeCount = 1,
      .pPoolSizes = &blocks};
  VkDescriptorPool made = VK_NULL_HANDLE;
  check(vkCreateDescriptorPool(_device, &info, nullptr, &made), "vkCreateDescriptorPool");
  _pools.push_back({.handle = made});
  return _pools.size() - 1;
}

std::uint32_t Pipelines::take_slot(VkImageView view) const {
  if (_free.empty())
    throw std::runtime_error(
        std::format("textures[] holds {} images, all taken", image_slots - 1));
  const std::uint32_t slot = _free.back();
  _free.pop_back();
  const VkDescriptorImageInfo image{
      .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = _image_set,
                                   .dstBinding = textures_binding,
                                   .dstArrayElement = slot,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   .pImageInfo = &image};
  vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
  return slot;
}

Pipeline::Pipeline(const Pipelines &pipelines, const Shader &compute)
    : _device(pipelines._device) {
  pipelines.take_frame(compute);
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
  pipelines.take_frame(vertex);
  pipelines.take_frame(fragment);
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
  // One blend mode, premultiplied over, so draws stack in graph order with no word for
  // it: a draw covers what came before by its alpha.
  const VkPipelineColorBlendAttachmentState color{
      .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
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
          size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Memory::upload)),
      _pool(pipelines.pool()) {
  const VkDescriptorSetAllocateInfo allocate{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pipelines._pools[_pool].handle,
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
  ++pipelines._pools[_pool].blocks;
}

PassBlock::PassBlock(PassBlock &&other) noexcept
    : _pipelines(other._pipelines), _buffer(std::move(other._buffer)), _pool(other._pool),
      _set(std::exchange(other._set, VK_NULL_HANDLE)) {}

PassBlock &PassBlock::operator=(PassBlock &&other) noexcept {
  std::swap(_pipelines, other._pipelines);
  std::swap(_buffer, other._buffer);
  std::swap(_pool, other._pool);
  std::swap(_set, other._set);
  return *this;
}

PassBlock::~PassBlock() {
  if (!_set)
    return;
  Pipelines::Pool &pool = _pipelines->_pools[_pool];
  vkFreeDescriptorSets(_pipelines->_device, pool.handle, 1, &_set);
  --pool.blocks;
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

Sampled::Sampled(const Pipelines &pipelines, Image image)
    : _pipelines(&pipelines), _image(std::move(image)),
      _slot(pipelines.take_slot(_image.view())) {}

Sampled::Sampled(Sampled &&other) noexcept
    : _pipelines(other._pipelines), _image(std::move(other._image)),
      _slot(std::exchange(other._slot, 0)) {}

Sampled &Sampled::operator=(Sampled &&other) noexcept {
  std::swap(_pipelines, other._pipelines);
  std::swap(_image, other._image);
  std::swap(_slot, other._slot);
  return *this;
}

// Between frames, so no pass that samples the slot is still running; until a pass block
// names it again, nothing reads it.
Sampled::~Sampled() {
  if (_slot != 0)
    _pipelines->_free.push_back(_slot);
}

const Image &Sampled::image() const {
  return _image;
}

std::uint32_t Sampled::slot() const {
  return _slot;
}

Drawn::Drawn(const Pipelines &pipelines, Image image)
    : _sampled(pipelines, std::move(image)), _device(pipelines._device) {
  const Image &drawn = _sampled.image();
  const VkImageView view = drawn.view();
  const VkFramebufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                     .renderPass = pipelines.render_pass(drawn.format()),
                                     .attachmentCount = 1,
                                     .pAttachments = &view,
                                     .width = drawn.extent().width,
                                     .height = drawn.extent().height,
                                     .layers = 1};
  check(vkCreateFramebuffer(_device, &info, nullptr, &_framebuffer),
        "vkCreateFramebuffer");
}

Drawn::Drawn(Drawn &&other) noexcept
    : _sampled(std::move(other._sampled)), _device(other._device),
      _framebuffer(std::exchange(other._framebuffer, VK_NULL_HANDLE)) {}

Drawn &Drawn::operator=(Drawn &&other) noexcept {
  std::swap(_sampled, other._sampled);
  std::swap(_device, other._device);
  std::swap(_framebuffer, other._framebuffer);
  return *this;
}

// Between frames, as its image goes, so no draw through it is still running.
Drawn::~Drawn() {
  if (_framebuffer)
    vkDestroyFramebuffer(_device, _framebuffer, nullptr);
}

const Sampled &Drawn::sampled() const {
  return _sampled;
}

VkFramebuffer Drawn::framebuffer() const {
  return _framebuffer;
}

} // namespace VP
