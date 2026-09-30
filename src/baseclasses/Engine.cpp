#include "baseclasses/Engine.h"

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

void bind(VkCommandBuffer commands, VkPipelineLayout layout, const Pass &pass) {
  vkCmdBindPipeline(commands, pass.bind_point, pass.pipeline);
  if (pass.block)
    vkCmdBindDescriptorSets(
        commands, pass.bind_point, layout, pass_set, 1, &pass.block, 0, nullptr);
}

} // namespace

Engine::Engine(const Log &log, const Window *window)
    : _mechanics(log, window),
      _resources(
          _mechanics.instance(), _mechanics.physical_device(), _mechanics.device()),
      _pipelines(_mechanics.device(), _resources) {
  if (window)
    _swapchain.emplace(log,
                       _mechanics.physical_device(),
                       _mechanics.device(),
                       _mechanics.queue(),
                       _mechanics.surface(),
                       *window);
}

Engine::~Engine() {
  _mechanics.wait_idle();
}

const Resources &Engine::resources() const {
  return _resources;
}

const Pipelines &Engine::pipelines() const {
  return _pipelines;
}

VkRenderPass Engine::render_pass() const {
  return _swapchain ? _swapchain->render_pass() : VK_NULL_HANDLE;
}

void Engine::wait() const {
  _mechanics.wait();
}

void Engine::run(std::span<const VkBuffer> clears, std::span<const Pass> passes) {
  const VkCommandBuffer commands = _mechanics.record();
  for (const VkBuffer buffer : clears)
    vkCmdFillBuffer(commands, buffer, 0, VK_WHOLE_SIZE, 0);
  if (!clears.empty())
    barrier(commands,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  dispatch(commands, passes);
  const std::optional<Target> target =
      _swapchain ? _swapchain->acquire() : std::optional<Target>{};
  if (target)
    draw(commands, *target, passes);
  // Readbacks: the CPU reads this frame's writes after its fence (VK03).
  barrier(commands,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_HOST_READ_BIT);
  if (!target) {
    _mechanics.submit();
    return;
  }
  _mechanics.submit(target->acquired, target->rendered);
  _swapchain->present(*target);
}

void Engine::dispatch(VkCommandBuffer commands, std::span<const Pass> passes) {
  _hazards.clear();
  for (const Pass &pass : passes) {
    if (pass.bind_point != VK_PIPELINE_BIND_POINT_COMPUTE)
      continue;
    if (_hazards.before(pass))
      barrier(commands,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    bind(commands, _pipelines.layout(), pass);
    vkCmdDispatch(commands, pass.groups, 1, 1);
  }
}

// Draws read what the clears and dispatches wrote; the CPU's uploads are visible to the
// GPU once the frame is submitted.
void Engine::draw(VkCommandBuffer commands,
                  const Target &target,
                  std::span<const Pass> passes) const {
  barrier(commands,
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT);
  _swapchain->begin(commands, target);
  for (const Pass &pass : passes)
    if (pass.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      bind(commands, _pipelines.layout(), pass);
      vkCmdDraw(commands, pass.vertex_count, 1, 0, 0);
    }
  vkCmdEndRenderPass(commands);
}

} // namespace VP
