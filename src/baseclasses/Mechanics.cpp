#include "baseclasses/Mechanics.h"

#include "baseclasses/Fail.h"
#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VP {

namespace {

// "Where there is neither Greek nor Jew, circumcision nor uncircumcision, Barbarian,
// Scythian, bond nor free: but Christ is all, and in all." Colossians 3:11

constexpr auto no_timeout = std::numeric_limits<std::uint64_t>::max();
constexpr float queue_priority = 1.0f;
constexpr const char *validation_layer = "VK_LAYER_KHRONOS_validation";
// How the instance extension of every kind of surface ends: VK_KHR_surface, and each
// platform's, such as VK_KHR_xcb_surface.
constexpr std::string_view surface_extension = "_surface";
#ifdef NDEBUG
constexpr bool validate = false;
#else
// Debug runs are checked when the layer is installed (RVK00).
constexpr bool validate = true;
#endif

// Best first: the fastest device that meets the floor runs the view (C03).
constexpr std::array preference{VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                                VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                                VK_PHYSICAL_DEVICE_TYPE_CPU};
// VkPhysicalDeviceType's names, by value.
constexpr std::array device_types{std::string_view{"other"},
                                  std::string_view{"integrated"},
                                  std::string_view{"discrete"},
                                  std::string_view{"virtual"},
                                  std::string_view{"cpu"}};

bool layer_installed(const char *name) {
  const auto layers = enumerated(vkEnumerateInstanceLayerProperties);
  return std::ranges::any_of(layers, [&](const VkLayerProperties &layer) {
    return std::strcmp(layer.layerName, name) == 0;
  });
}

// Every surface extension the loader offers, so a window can open whenever a draw
// appears, while starting needs no display (V07).
VkInstance create_instance(const Log &log) {
  const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                      .pApplicationName = "vulpen",
                                      .apiVersion = VK_API_VERSION_1_2};
  const bool validated = validate && layer_installed(validation_layer);
  const auto offered = enumerated(vkEnumerateInstanceExtensionProperties, nullptr);
  std::vector<const char *> extensions;
  for (const VkExtensionProperties &extension : offered)
    if (std::string_view(extension.extensionName).ends_with(surface_extension))
      extensions.push_back(extension.extensionName);
  const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                  .pApplicationInfo = &application,
                                  .enabledLayerCount = validated ? 1u : 0u,
                                  .ppEnabledLayerNames = &validation_layer,
                                  .enabledExtensionCount =
                                      static_cast<std::uint32_t>(extensions.size()),
                                  .ppEnabledExtensionNames = extensions.data()};
  VkInstance instance = VK_NULL_HANDLE;
  check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
  if constexpr (validate)
    log.write(Level::info,
              Tag::gpu,
              validated ? std::string("Vulkan validation is on")
                        : std::format("Vulkan validation is off: {} is not installed",
                                      validation_layer));
  return instance;
}

// The Vulkan floor (RV01): 1.2 with buffer device address and descriptor indexing.
bool meets_floor(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  VkPhysicalDeviceVulkan12Features features12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures2 features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
  vkGetPhysicalDeviceFeatures2(device, &features);
  return properties.apiVersion >= VK_API_VERSION_1_2 && features12.descriptorIndexing &&
         features.features.shaderSampledImageArrayDynamicIndexing &&
         features12.bufferDeviceAddress;
}

bool has_extension(VkPhysicalDevice device, const char *name) {
  const auto extensions =
      enumerated(vkEnumerateDeviceExtensionProperties, device, nullptr);
  return std::ranges::any_of(extensions, [&](const VkExtensionProperties &extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

bool has_swapchain(VkPhysicalDevice device) {
  return has_extension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

std::vector<VkQueueFamilyProperties> families_of(VkPhysicalDevice device) {
  return enumerated(vkGetPhysicalDeviceQueueFamilyProperties, device);
}

// One queue runs everything: compute, and draws where the device can, so a window can
// open when a draw appears. With a surface, the queue must present to it.
std::optional<std::uint32_t> queue_family(VkPhysicalDevice device, VkSurfaceKHR surface) {
  constexpr std::array<VkQueueFlags, 2> wanted{
      VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_COMPUTE_BIT};
  const std::vector<VkQueueFamilyProperties> families = families_of(device);
  const auto count = static_cast<std::uint32_t>(families.size());
  for (const VkQueueFlags needed : std::span(wanted).first(surface ? 1 : wanted.size()))
    for (std::uint32_t index = 0; index < count; ++index) {
      VkBool32 present = VK_TRUE;
      if (surface)
        vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &present);
      if ((families[index].queueFlags & needed) == needed && present)
        return index;
    }
  return std::nullopt;
}

// Why a device cannot run the view; null when it can.
const char *unfit(VkPhysicalDevice device, VkSurfaceKHR surface) {
  if (!meets_floor(device))
    return "is below the Vulkan floor";
  if (surface && !has_swapchain(device))
    return "cannot present to the window";
  if (!queue_family(device, surface))
    return "has no queue that runs the view";
  return nullptr;
}

std::string_view kind_of(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  const auto type = static_cast<std::size_t>(properties.deviceType);
  return type < device_types.size() ? device_types[type] : device_types[0];
}

// As the log names a device: "name: type, Vulkan 1.3.280".
std::string describe(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  return std::format("{}: {}, Vulkan {}.{}.{}",
                     properties.deviceName,
                     kind_of(device),
                     VK_API_VERSION_MAJOR(properties.apiVersion),
                     VK_API_VERSION_MINOR(properties.apiVersion),
                     VK_API_VERSION_PATCH(properties.apiVersion));
}

// The kind a person prefers first, then the fastest.
std::size_t rank(VkPhysicalDevice device, std::string_view prefer) {
  if (!prefer.empty() && kind_of(device) == prefer)
    return 0;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  return 1 +
         static_cast<std::size_t>(std::ranges::find(preference, properties.deviceType) -
                                  preference.begin());
}

// The GPUs that meet the floor, and present to the window where there is one.
std::vector<VkPhysicalDevice>
fit_devices(VkInstance instance, VkSurfaceKHR surface, const Log &log) {
  auto devices = enumerated(vkEnumeratePhysicalDevices, instance);
  std::erase_if(devices, [&](VkPhysicalDevice device) {
    const char *const reason = unfit(device, surface);
    log.write(
        Level::debug,
        Tag::gpu,
        std::format("{}; {}", describe(device), reason ? reason : "meets the floor"));
    return reason != nullptr;
  });
  if (devices.empty())
    fail("no GPU meets the Vulkan floor: 1.2 with buffer device address and "
         "descriptor indexing (RV01){}; --log debug says why for each GPU",
         surface ? ", and presents to the window" : "");
  return devices;
}

VkPhysicalDevice best_of(std::span<const VkPhysicalDevice> devices,
                         std::string_view prefer) {
  return *std::ranges::min_element(
      devices, {}, [&](VkPhysicalDevice device) { return rank(device, prefer); });
}

VkDevice create_device(VkPhysicalDevice physical_device, std::uint32_t family) {
  const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueFamilyIndex = family,
                                      .queueCount = 1,
                                      .pQueuePriorities = &queue_priority};
  // What set 0's arrays of images need (RV02), all of which descriptor indexing brings
  // but indexing images[], which a GPU without it leaves to no node (Resources::samples);
  // and a draw's geometry and tessellation stages, which a GPU without them refuses
  // (Resources::runs).
  VkPhysicalDeviceFeatures supported{};
  vkGetPhysicalDeviceFeatures(physical_device, &supported);
  const VkPhysicalDeviceFeatures features{
      .geometryShader = supported.geometryShader,
      .tessellationShader = supported.tessellationShader,
      .shaderSampledImageArrayDynamicIndexing = VK_TRUE,
      .shaderStorageImageArrayDynamicIndexing =
          supported.shaderStorageImageArrayDynamicIndexing};
  const VkPhysicalDeviceVulkan12Features features12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .descriptorIndexing = VK_TRUE,
      .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
      .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
      .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
      .descriptorBindingPartiallyBound = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
      .bufferDeviceAddress = VK_TRUE};
  // What the GPU has free, which every allocation stays within, where the GPU tells it,
  // so a count it cannot hold is refused before any memory is taken (A02, A03).
  std::vector<const char *> extensions;
  for (const char *const name :
       {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME})
    if (has_extension(physical_device, name))
      extensions.push_back(name);
  const VkDeviceCreateInfo info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                .pNext = &features12,
                                .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &queue,
                                .enabledExtensionCount =
                                    static_cast<std::uint32_t>(extensions.size()),
                                .ppEnabledExtensionNames = extensions.data(),
                                .pEnabledFeatures = &features};
  VkDevice device = VK_NULL_HANDLE;
  check(vkCreateDevice(physical_device, &info, nullptr, &device), "vkCreateDevice");
  return device;
}

} // namespace

void check(VkResult result, const char *call) {
  if (result != VK_SUCCESS)
    fail("{} failed: VkResult {}", call, int{result});
}

// A drawn image starts each frame cleared, so what it held before is dropped. Once
// drawn, every shader stage that samples it after its render pass sees its pixels, in
// the layout sampling takes; earlier frames are behind the fence already. A window's
// image takes no dependency after its pass: its present waits on the semaphore the
// frame's submit signals, and its acquire on the color stage, before the pass. A depth
// image clears to the far plane; dropped once the pass ends, a tile-based GPU never
// writes it out, and kept, the passes after it sample it as they sample the colors. The
// last frame's tests on it finish before this frame's start. With samples past one, the
// draws go into a color of that many samples, which the pass resolves into the image as
// it ends and then drops, as it drops the depth: a kept depth would need resolving too.
VkRenderPass make_render_pass(VkDevice device,
                              VkFormat format,
                              VkFormat depth_format,
                              bool depth_kept,
                              VkSampleCountFlagBits samples,
                              VkImageLayout ends) {
  const bool depth = depth_format != VK_FORMAT_UNDEFINED;
  const bool kept = depth && depth_kept;
  const bool resolves = samples != VK_SAMPLE_COUNT_1_BIT;
  const bool sampled = ends == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (kept && resolves)
    throw std::runtime_error("a depth that later passes sample is drawn with one sample "
                             "a pixel");
  const VkAttachmentDescription resolved{.format = format,
                                         .samples = VK_SAMPLE_COUNT_1_BIT,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .stencilStoreOp =
                                             VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                         .finalLayout = ends};
  std::array attachments{
      VkAttachmentDescription{
          .format = format,
          .samples = samples,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp =
              resolves ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = resolves ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : ends},
      VkAttachmentDescription{
          .format = depth_format,
          .samples = samples,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp =
              kept ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = kept ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
      resolved};
  // The color, then the depth and the resolve where there are: no gap between them.
  const std::uint32_t at_resolve = depth ? 2U : 1U;
  attachments[at_resolve] = resolved;
  const VkAttachmentReference target{.attachment = 0,
                                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkAttachmentReference tested{
      .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  const VkAttachmentReference into{.attachment = at_resolve,
                                   .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentCount = 1,
                                     .pColorAttachments = &target,
                                     .pResolveAttachments = resolves ? &into : nullptr,
                                     .pDepthStencilAttachment =
                                         depth ? &tested : nullptr};
  const VkPipelineStageFlags tests = depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                           : 0;
  const VkAccessFlags written = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0;
  const VkPipelineStageFlags kept_tests =
      kept ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : 0;
  const VkAccessFlags kept_written = kept ? written : 0;
  const std::array dependencies{
      VkSubpassDependency{
          .srcSubpass = VK_SUBPASS_EXTERNAL,
          .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | tests,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | tests,
          .srcAccessMask = written,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | written},
      VkSubpassDependency{
          .srcSubpass = 0,
          .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kept_tests,
          .dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | kept_written,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
  const VkRenderPassCreateInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                    .attachmentCount = at_resolve + (resolves ? 1U : 0U),
                                    .pAttachments = attachments.data(),
                                    .subpassCount = 1,
                                    .pSubpasses = &subpass,
                                    .dependencyCount = sampled ? 2U : 1U,
                                    .pDependencies = dependencies.data()};
  VkRenderPass render_pass = VK_NULL_HANDLE;
  check(vkCreateRenderPass(device, &info, nullptr, &render_pass), "vkCreateRenderPass");
  return render_pass;
}

Mechanics::Mechanics(const Log &log, const Window *window, std::string_view prefer)
    : _instance(create_instance(log)), _choice(choose(_instance, window, log, prefer)),
      _device(create_device(_choice.device, _choice.family)) {
  vkGetDeviceQueue(_device, _choice.family, 0, &_queue);
  const VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags =
                                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                     .queueFamilyIndex = _choice.family};
  VkCommandPool made_pool = VK_NULL_HANDLE;
  check(vkCreateCommandPool(_device, &pool, nullptr, &made_pool), "vkCreateCommandPool");
  _command_pool = {_device, made_pool};
  const VkCommandBufferAllocateInfo commands{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = _command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  check(vkAllocateCommandBuffers(_device, &commands, &_commands),
        "vkAllocateCommandBuffers");
  // Signalled, so the first frame's wait returns at once.
  const VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                .flags = VK_FENCE_CREATE_SIGNALED_BIT};
  VkFence made_fence = VK_NULL_HANDLE;
  check(vkCreateFence(_device, &fence, nullptr, &made_fence), "vkCreateFence");
  _fence = {_device, made_fence};
}

// The GPU is done with everything before the members go.
Mechanics::~Mechanics() {
  vkDeviceWaitIdle(_device);
}

VkInstance Mechanics::instance() const {
  return _instance;
}

VkPhysicalDevice Mechanics::physical_device() const {
  return _choice.device;
}

bool Mechanics::budget() const {
  return has_extension(_choice.device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
}

VkDevice Mechanics::device() const {
  return _device;
}

VkQueue Mechanics::queue() const {
  return _queue;
}

// A device chosen headless may have only a queue that computes, which cannot draw.
bool Mechanics::presents(VkSurfaceKHR surface) const {
  VkBool32 present = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(_choice.device, _choice.family, surface, &present);
  return present && has_swapchain(_choice.device) &&
         (families_of(_choice.device)[_choice.family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

float Mechanics::timestamp_period() const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(_choice.device, &properties);
  return properties.limits.timestampPeriod;
}

std::uint32_t Mechanics::timestamp_bits() const {
  return families_of(_choice.device)[_choice.family].timestampValidBits;
}

// A window at start gets a device and queue that present to it, through a surface made
// only for the choice: the window's output makes its own.
Mechanics::Choice Mechanics::choose(VkInstance instance,
                                    const Window *window,
                                    const Log &log,
                                    std::string_view prefer) {
  const Owned<VkSurfaceKHR, vkDestroySurfaceKHR, VkInstance> surface{
      instance, window ? window->surface(instance) : VK_NULL_HANDLE};
  std::vector<VkPhysicalDevice> fit = fit_devices(instance, surface, log);
  const VkPhysicalDevice device = best_of(fit, prefer);
  log.write(Level::info, Tag::gpu, "runs on " + VP::describe(device));
  Choice choice{
      .device = device, .family = *queue_family(device, surface), .fit = std::move(fit)};
  return choice;
}

std::string Mechanics::describe() const {
  return VP::describe(_choice.device);
}

std::string_view Mechanics::kind() const {
  return kind_of(_choice.device);
}

std::string_view Mechanics::takes(std::string_view prefer) const {
  return kind_of(best_of(_choice.fit, prefer));
}

void Mechanics::wait() const {
  const VkFence fence = _fence;
  check(vkWaitForFences(_device, 1, &fence, VK_TRUE, no_timeout), "vkWaitForFences");
}

void Mechanics::wait_idle() const {
  check(vkDeviceWaitIdle(_device), "vkDeviceWaitIdle");
}

VkCommandBuffer Mechanics::record() const {
  check(vkResetCommandBuffer(_commands, 0), "vkResetCommandBuffer");
  const VkCommandBufferBeginInfo begin{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  check(vkBeginCommandBuffer(_commands, &begin), "vkBeginCommandBuffer");
  return _commands;
}

void Mechanics::submit(VkSemaphore acquired, VkSemaphore rendered) const {
  check(vkEndCommandBuffer(_commands), "vkEndCommandBuffer");
  const VkFence fence = _fence;
  check(vkResetFences(_device, 1, &fence), "vkResetFences");
  const VkPipelineStageFlags written = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .waitSemaphoreCount = acquired ? 1u : 0u,
                            .pWaitSemaphores = &acquired,
                            .pWaitDstStageMask = &written,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &_commands,
                            .signalSemaphoreCount = rendered ? 1u : 0u,
                            .pSignalSemaphores = &rendered};
  check(vkQueueSubmit(_queue, 1, &submit, _fence), "vkQueueSubmit");
}

} // namespace VP
