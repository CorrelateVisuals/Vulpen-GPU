#include "baseclasses/Engine.h"

#include "baseclasses/Mechanics.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Resources.h"
#include "baseclasses/Swapchain.h"

#include <limits>
#include <optional>
#include <vector>

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
constexpr double nanoseconds_per_millisecond = 1e6;
// A timestamp's value, then whether it is there, as the query pool hands them back.
constexpr std::size_t per_query = 2;

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
        pipelines(mechanics.device(), resources),
        tick(mechanics.timestamp_bits() == 0
                 ? 0.0
                 : mechanics.timestamp_period() / nanoseconds_per_millisecond),
        valid(mechanics.timestamp_bits() >= 64
                  ? ~std::uint64_t{0}
                  : (std::uint64_t{1} << mechanics.timestamp_bits()) - 1) {
    if (window)
      swapchain.emplace(log, mechanics, *window);
  }
  ~Gpu() {
    vkDestroyQueryPool(mechanics.device(), queries, nullptr);
  }
  Gpu(const Gpu &) = delete;
  Gpu &operator=(const Gpu &) = delete;

  void record(VkCommandBuffer commands, std::span<const Pass> passes);
  void draw(VkCommandBuffer commands,
            const Target &target,
            std::span<const Pass> passes) const;
  void begin_timing(VkCommandBuffer commands, std::size_t passes);
  void stamp(VkCommandBuffer commands, std::size_t pass, bool after) const;
  void read_times();

  const Log &log;
  Mechanics mechanics;
  Resources resources;
  Pipelines pipelines;
  std::optional<Swapchain> swapchain;
  Hazards hazards;
  // Two timestamps a pass, read back once the frame's fence passed (VK02); none where
  // the queue keeps no time.
  const double tick;         // milliseconds a timestamp's tick is
  const std::uint64_t valid; // the bits of a timestamp that count
  VkQueryPool queries = VK_NULL_HANDLE;
  std::uint32_t query_room = 0; // timestamps the pool holds
  std::uint32_t queried = 0;    // passes the last run timed, until they are read
  // Kept between frames, so a frame allocates nothing for them (CPP10).
  std::vector<std::uint64_t> stamps;
  std::vector<float> times;
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

void Engine::wait() {
  _gpu->mechanics.wait();
  _gpu->read_times();
}

std::span<const float> Engine::times() const {
  return _gpu->times;
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
  _gpu->begin_timing(commands, passes.size());
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

// In order, so each pass runs after those that write what it reads. The draws that
// follow one another into the same image record in one render pass, even with no
// instances, so the image is cleared and ready to sample; draws into the window wait for
// the window's. Draws into an image with no size yet record nothing, which takes no time.
void Engine::Gpu::record(VkCommandBuffer commands, std::span<const Pass> passes) {
  hazards.clear();
  for (std::size_t first = 0; first < passes.size();) {
    const Pass &pass = passes[first];
    const bool draw = pass.bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::size_t end = first + 1;
    while (draw && end < passes.size() && passes[end].bind_point == pass.bind_point &&
           passes[end].offscreen == pass.offscreen)
      ++end;
    const std::size_t at = first;
    const std::span<const Pass> run = passes.subspan(first, end - first);
    first = end;
    if (draw && !pass.offscreen)
      continue;
    const bool records = !draw || pass.offscreen->framebuffer;
    if (records && hazards.before(run))
      barrier(commands,
              shader_stages,
              VK_ACCESS_SHADER_WRITE_BIT,
              shader_stages,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (records && draw)
      begin(commands, *pass.offscreen);
    for (std::size_t each = 0; each < run.size(); ++each) {
      stamp(commands, at + each, false);
      if (records)
        bind(commands, pipelines, run[each]);
      if (records && !draw)
        vkCmdDispatch(commands, run[each].groups, 1, 1);
      else if (records && instances_of(run[each]) != 0)
        vkCmdDraw(commands, run[each].vertex_count, instances_of(run[each]), 0, 0);
      stamp(commands, at + each, true);
    }
    if (records && draw)
      vkCmdEndRenderPass(commands);
  }
}

// Room for two timestamps a pass, made anew when the passes outgrow it, which the
// frame's fence made safe, and reset before the frame writes them.
void Engine::Gpu::begin_timing(VkCommandBuffer commands, std::size_t passes) {
  queried = tick == 0.0 ? 0 : static_cast<std::uint32_t>(passes);
  const std::uint32_t needed = 2 * queried;
  if (needed == 0)
    return;
  if (needed > query_room) {
    vkDestroyQueryPool(mechanics.device(), queries, nullptr);
    queries = VK_NULL_HANDLE;
    const VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = needed};
    check(vkCreateQueryPool(mechanics.device(), &info, nullptr, &queries),
          "vkCreateQueryPool");
    query_room = needed;
  }
  vkCmdResetQueryPool(commands, queries, 0, needed);
}

// The first as the GPU takes the pass up, the second once it and every command before it
// are done.
void Engine::Gpu::stamp(VkCommandBuffer commands, std::size_t pass, bool after) const {
  if (queried != 0)
    vkCmdWriteTimestamp(commands,
                        after ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                              : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        queries,
                        static_cast<std::uint32_t>(2 * pass + (after ? 1 : 0)));
}

// Once the frame's fence passed, so no read waits (VK02): a pass took the time between its
// two timestamps where both are there, the tick's bits that count wrapping round.
void Engine::Gpu::read_times() {
  times.assign(queried, std::numeric_limits<float>::quiet_NaN());
  if (queried == 0)
    return;
  stamps.resize(2 * per_query * queried);
  const VkResult result =
      vkGetQueryPoolResults(mechanics.device(),
                            queries,
                            0,
                            2 * queried,
                            stamps.size() * sizeof(std::uint64_t),
                            stamps.data(),
                            per_query * sizeof(std::uint64_t),
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (result != VK_NOT_READY)
    check(result, "vkGetQueryPoolResults");
  for (std::uint32_t pass = 0; pass < queried; ++pass) {
    const std::uint64_t *const before = &stamps[2 * per_query * pass];
    const std::uint64_t *const after = before + per_query;
    if (before[1] != 0 && after[1] != 0)
      times[pass] = static_cast<float>(
          static_cast<double>((after[0] - before[0]) & valid) * tick);
  }
  queried = 0;
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
  for (std::size_t at = 0; at < passes.size(); ++at) {
    const Pass &pass = passes[at];
    if (pass.bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS || pass.offscreen)
      continue;
    stamp(commands, at, false);
    if (instances_of(pass) != 0) {
      bind(commands, pipelines, pass);
      vkCmdDraw(commands, pass.vertex_count, instances_of(pass), 0, 0);
    }
    stamp(commands, at, true);
  }
  vkCmdEndRenderPass(commands);
}

} // namespace VP
