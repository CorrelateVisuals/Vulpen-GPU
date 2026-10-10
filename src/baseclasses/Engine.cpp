#include "baseclasses/Engine.h"

#include "baseclasses/Mechanics.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Resources.h"
#include "baseclasses/Swapchain.h"

#include <optional>

namespace VP {

namespace {

// The stages that reach buffers, so a pass after a dispatch or a draw waits for it
// whichever it is.
constexpr VkPipelineStageFlags shader_stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
// A draw's image starts as nothing at all, so what the draw leaves uncovered samples as
// transparent.
constexpr VkClearColorValue transparent{.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
constexpr float far_depth = 1.0f;

void barrier(VkCommandBuffer commands,
             VkPipelineStageFlags source_stage,
             VkAccessFlags source,
             VkPipelineStageFlags target_stage,
             VkAccessFlags target) {
  const VkMemoryBarrier memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = source,
                               .dstAccessMask = target};
  vkCmdPipelineBarrier(
      commands, source_stage, target_stage, 0, 1, &memory, 0, nullptr, 0, nullptr);
}

void bind(VkCommandBuffer commands, const Pipelines &pipelines, const Pass &pass) {
  const VkPipelineLayout layout = pipelines.layout();
  const VkDeviceAddress frame = pipelines.frame();
  const VkDescriptorSet images = pipelines.images();
  vkCmdBindPipeline(commands, pass.bind_point, pass.pipeline);
  vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_ALL, 0, sizeof frame, &frame);
  vkCmdBindDescriptorSets(
      commands, pass.bind_point, layout, images_set, 1, &images, 0, nullptr);
  if (pass.block)
    vkCmdBindDescriptorSets(
        commands, pass.bind_point, layout, pass_set, 1, &pass.block, 0, nullptr);
}

std::uint32_t instances_of(const Pass &pass) {
  return pass.instances ? *pass.instances : pass.instance_count;
}

// The viewport and the scissor cover the image a render pass draws into.
void fill(VkCommandBuffer commands, VkExtent2D extent) {
  const VkViewport viewport{.width = static_cast<float>(extent.width),
                            .height = static_cast<float>(extent.height),
                            .maxDepth = far_depth};
  const VkRect2D scissor{.extent = extent};
  vkCmdSetViewport(commands, 0, 1, &viewport);
  vkCmdSetScissor(commands, 0, 1, &scissor);
}

void begin(VkCommandBuffer commands, const Offscreen &offscreen) {
  const VkClearValue clear{.color = transparent};
  const VkRenderPassBeginInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = offscreen.render_pass,
                                   .framebuffer = offscreen.framebuffer,
                                   .renderArea = {.extent = offscreen.extent},
                                   .clearValueCount = 1,
                                   .pClearValues = &clear};
  vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
  fill(commands, offscreen.extent);
}

// The image's old pixels go, so the copy waits for nothing; uploads are rare, so what
// runs after it waits for it, whatever that is, rather than naming the stages that
// sample.
void upload(VkCommandBuffer commands, const Copy &copy) {
  VkImageMemoryBarrier layout{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = copy.to,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
  const auto transition = [&](VkPipelineStageFlags source, VkPipelineStageFlags target) {
    vkCmdPipelineBarrier(commands, source, target, 0, 0, nullptr, 0, nullptr, 1, &layout);
  };
  transition(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  const VkBufferImageCopy region{
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
      .imageExtent = {copy.extent.width, copy.extent.height, 1}};
  vkCmdCopyBufferToImage(
      commands, copy.from, copy.to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  layout.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  layout.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  layout.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  layout.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  transition(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

} // namespace

// Each part borrows the ones made before it, so they are destroyed in reverse.
struct Engine::Gpu {
  Gpu(const Log &log, const Window *window)
      : log(log), mechanics(log, window),
        resources(mechanics.instance(), mechanics.physical_device(), mechanics.device()),
        pipelines(mechanics.device(), resources) {
    if (window)
      swapchain.emplace(log, mechanics, *window);
  }

  void record(VkCommandBuffer commands, std::span<const Pass> passes);
  void draw(VkCommandBuffer commands,
            const Target &target,
            std::span<const Pass> passes) const;

  const Log &log;
  Mechanics mechanics;
  Resources resources;
  Pipelines pipelines;
  std::optional<Swapchain> swapchain;
  Hazards hazards;
};

Engine::Engine(const Log &log, const Window *window)
    : _gpu(std::make_unique<Gpu>(log, window)) {}

Engine::~Engine() {
  _gpu->mechanics.wait_idle();
}

const Resources &Engine::resources() const {
  return _gpu->resources;
}

const Pipelines &Engine::pipelines() const {
  return _gpu->pipelines;
}

void Engine::open(const Window &window) {
  _gpu->swapchain.emplace(_gpu->log, _gpu->mechanics, window);
}

void Engine::close() {
  _gpu->swapchain.reset();
}

VkRenderPass Engine::render_pass() const {
  return _gpu->swapchain ? _gpu->swapchain->render_pass() : VK_NULL_HANDLE;
}

void Engine::wait() const {
  _gpu->mechanics.wait();
}

void Engine::run(std::span<const VkBuffer> clears,
                 std::span<const Copy> copies,
                 std::span<const Pass> passes,
                 std::uint64_t frame,
                 double time,
                 std::array<float, 2> cursor,
                 VkExtent2D size) {
  Mechanics &mechanics = _gpu->mechanics;
  const VkCommandBuffer commands = mechanics.record();
  for (const VkBuffer buffer : clears)
    vkCmdFillBuffer(commands, buffer, 0, VK_WHOLE_SIZE, 0);
  if (!clears.empty())
    barrier(commands,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            shader_stages,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  for (const Copy &copy : copies)
    upload(commands, copy);
  _gpu->record(commands, passes);
  std::optional<Swapchain> &swapchain = _gpu->swapchain;
  const std::optional<Target> target =
      swapchain ? swapchain->acquire() : std::optional<Target>{};
  if (target)
    _gpu->draw(commands, *target, passes);
  // After acquiring, so a resized window's frame already holds its new size.
  _gpu->pipelines.write_frame(frame, time, target ? target->extent : size, cursor);
  // Readbacks: the CPU reads this frame's writes after its fence (VK03).
  barrier(commands,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_HOST_READ_BIT);
  if (!target) {
    mechanics.submit();
    return;
  }
  mechanics.submit(target->acquired, target->rendered);
  swapchain->present(*target);
}

// In graph order, so each pass runs after those that write what it reads. A draw into an
// image records a render pass of its own, even with no instances, so its image is
// cleared and ready to sample; a draw into the window waits for the window's.
void Engine::Gpu::record(VkCommandBuffer commands, std::span<const Pass> passes) {
  hazards.clear();
  for (const Pass &pass : passes) {
    const bool draw = pass.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (draw && (!pass.offscreen || !pass.offscreen->framebuffer))
      continue;
    if (hazards.before(pass))
      barrier(commands,
              shader_stages,
              VK_ACCESS_SHADER_WRITE_BIT,
              shader_stages,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (draw)
      begin(commands, *pass.offscreen);
    bind(commands, pipelines, pass);
    if (!draw)
      vkCmdDispatch(commands, pass.groups, 1, 1);
    else if (instances_of(pass) != 0)
      vkCmdDraw(commands, pass.vertex_count, instances_of(pass), 0, 0);
    if (draw)
      vkCmdEndRenderPass(commands);
  }
}

// Draws read what the clears and dispatches wrote; the CPU's uploads are visible to the
// GPU once the frame is submitted.
void Engine::Gpu::draw(VkCommandBuffer commands,
                       const Target &target,
                       std::span<const Pass> passes) const {
  barrier(commands,
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT);
  swapchain->begin(commands, target);
  fill(commands, target.extent);
  // A draw given nothing this frame records nothing, while its pipeline and buffers stay,
  // so what is hidden costs no draw and shows again the frame it is given something.
  for (const Pass &pass : passes) {
    if (pass.bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS || pass.offscreen ||
        instances_of(pass) == 0)
      continue;
    bind(commands, pipelines, pass);
    vkCmdDraw(commands, pass.vertex_count, instances_of(pass), 0, 0);
  }
  vkCmdEndRenderPass(commands);
}

} // namespace VP
