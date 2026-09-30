#include "baseclasses/Mechanics.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace VP {

namespace {

constexpr auto no_timeout = std::numeric_limits<std::uint64_t>::max();
constexpr float queue_priority = 1.0f;
constexpr const char *validation_layer = "VK_LAYER_KHRONOS_validation";
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

bool layer_installed(const char *name) {
  std::uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::ranges::any_of(layers, [&](const VkLayerProperties &layer) {
    return std::strcmp(layer.layerName, name) == 0;
  });
}

VkInstance create_instance(const Log &log, const Window *window) {
  const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                      .pApplicationName = "vulpen",
                                      .apiVersion = VK_API_VERSION_1_2};
  const bool validated = validate && layer_installed(validation_layer);
  const std::span<const char *const> extensions =
      window ? window->vulkan_extensions() : std::span<const char *const>{};
  const VkInstanceCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
      .enabledLayerCount = validated ? 1u : 0u,
      .ppEnabledLayerNames = &validation_layer,
      .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
      .ppEnabledExtensionNames = extensions.data()};
  VkInstance instance = VK_NULL_HANDLE;
  check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
  if (validated)
    log.write(Level::info, "Vulkan validation is on");
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
         features12.bufferDeviceAddress;
}

bool presents(VkPhysicalDevice device) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
  return std::ranges::any_of(extensions, [](const VkExtensionProperties &extension) {
    return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
  });
}

// One queue runs everything: compute, and with a window also draws and presents.
std::optional<std::uint32_t> queue_family(VkPhysicalDevice device, VkSurfaceKHR surface) {
  const VkQueueFlags needed =
      surface ? VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT;
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
  for (std::uint32_t index = 0; index < count; ++index) {
    VkBool32 present = VK_TRUE;
    if (surface)
      vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &present);
    if ((families[index].queueFlags & needed) == needed && present)
      return index;
  }
  return std::nullopt;
}

std::size_t rank(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  return static_cast<std::size_t>(std::ranges::find(preference, properties.deviceType) -
                                  preference.begin());
}

VkPhysicalDevice pick_device(VkInstance instance, VkSurfaceKHR surface, const Log &log) {
  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  std::erase_if(devices, [&](VkPhysicalDevice device) {
    return !meets_floor(device) || (surface && !presents(device)) ||
           !queue_family(device, surface);
  });
  if (devices.empty())
    throw std::runtime_error(
        std::format("no GPU meets the Vulkan floor: 1.2 with buffer device address and "
                    "descriptor indexing (RV01){}",
                    surface ? ", and presents to the window" : ""));
  const VkPhysicalDevice best = *std::ranges::min_element(devices, {}, rank);
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(best, &properties);
  log.write(Level::info, std::format("GPU: {}", properties.deviceName));
  return best;
}

VkDevice create_device(VkPhysicalDevice physical_device,
                       std::uint32_t family,
                       VkSurfaceKHR surface) {
  const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueFamilyIndex = family,
                                      .queueCount = 1,
                                      .pQueuePriorities = &queue_priority};
  const VkPhysicalDeviceVulkan12Features features12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .descriptorIndexing = VK_TRUE,
      .bufferDeviceAddress = VK_TRUE};
  const char *const swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  const VkDeviceCreateInfo info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                .pNext = &features12,
                                .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &queue,
                                .enabledExtensionCount = surface ? 1u : 0u,
                                .ppEnabledExtensionNames = &swapchain};
  VkDevice device = VK_NULL_HANDLE;
  check(vkCreateDevice(physical_device, &info, nullptr, &device), "vkCreateDevice");
  return device;
}

} // namespace

void check(VkResult result, const char *call) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::format("{} failed: VkResult {}", call, int{result}));
}

Mechanics::Mechanics(const Log &log, const Window *window)
    : _instance(create_instance(log, window)),
      _surface(window ? window->surface(_instance) : VK_NULL_HANDLE),
      _physical_device(pick_device(_instance, _surface, log)),
      _queue_family(*queue_family(_physical_device, _surface)),
      _device(create_device(_physical_device, _queue_family, _surface)) {
  vkGetDeviceQueue(_device, _queue_family, 0, &_queue);
  const VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags =
                                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                     .queueFamilyIndex = _queue_family};
  check(vkCreateCommandPool(_device, &pool, nullptr, &_command_pool),
        "vkCreateCommandPool");
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
  check(vkCreateFence(_device, &fence, nullptr, &_fence), "vkCreateFence");
}

Mechanics::~Mechanics() {
  vkDeviceWaitIdle(_device);
  vkDestroyFence(_device, _fence, nullptr);
  vkDestroyCommandPool(_device, _command_pool, nullptr);
  vkDestroyDevice(_device, nullptr);
  // Headless, the instance lacks VK_KHR_surface, so even a null surface is not passed.
  if (_surface)
    vkDestroySurfaceKHR(_instance, _surface, nullptr);
  vkDestroyInstance(_instance, nullptr);
}

VkInstance Mechanics::instance() const {
  return _instance;
}

VkPhysicalDevice Mechanics::physical_device() const {
  return _physical_device;
}

VkDevice Mechanics::device() const {
  return _device;
}

VkQueue Mechanics::queue() const {
  return _queue;
}

VkSurfaceKHR Mechanics::surface() const {
  return _surface;
}

void Mechanics::wait() const {
  check(vkWaitForFences(_device, 1, &_fence, VK_TRUE, no_timeout), "vkWaitForFences");
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
  check(vkResetFences(_device, 1, &_fence), "vkResetFences");
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
