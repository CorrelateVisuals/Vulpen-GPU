#include "baseclasses/Engine.h"

#include "baseclasses/Mechanics.h"
#include "baseclasses/Offscreen.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Resources.h"
#include "baseclasses/Swapchain.h"

#include <optional>

namespace VP {

namespace {

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
  vkCmdBindPipeline(commands, pass.bind_point, pass.pipeline);
  vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_ALL, 0, sizeof frame, &frame);
  if (pass.block)
    vkCmdBindDescriptorSets(
        commands, pass.bind_point, layout, pass_set, 1, &pass.block, 0, nullptr);
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

  void dispatch(VkCommandBuffer commands, std::span<const Pass> passes);
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
                 std::span<const Pass> passes,
                 std::uint64_t frame,
                 double time) {
  Mechanics &mechanics = _gpu->mechanics;
  const VkCommandBuffer commands = mechanics.record();
  for (const VkBuffer buffer : clears)
    vkCmdFillBuffer(commands, buffer, 0, VK_WHOLE_SIZE, 0);
  if (!clears.empty())
    barrier(commands,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  _gpu->dispatch(commands, passes);
  std::optional<Swapchain> &swapchain = _gpu->swapchain;
  const std::optional<Target> target =
      swapchain ? swapchain->acquire() : std::optional<Target>{};
  if (target)
    _gpu->draw(commands, *target, passes);
  // After acquiring, so a resized window's frame already holds its new size.
  _gpu->pipelines.write_frame(frame, time, target ? target->extent : VkExtent2D{});
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

void Engine::Gpu::dispatch(VkCommandBuffer commands, std::span<const Pass> passes) {
  hazards.clear();
  for (const Pass &pass : passes) {
    if (pass.bind_point != VK_PIPELINE_BIND_POINT_COMPUTE)
      continue;
    if (hazards.before(pass))
      barrier(commands,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    bind(commands, pipelines, pass);
    vkCmdDispatch(commands, pass.groups, 1, 1);
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
  for (const Pass &pass : passes)
    if (pass.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      bind(commands, pipelines, pass);
      vkCmdDraw(commands, pass.vertex_count, pass.instance_count, 0, 0);
    }
  vkCmdEndRenderPass(commands);
}

} // namespace VP
