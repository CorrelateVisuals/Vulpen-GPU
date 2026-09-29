#include "baseclasses/Mechanics.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
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
constexpr bool validate =
    true; // debug runs are checked when the layer is installed (RVK00)
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

VkInstance create_instance(const Log &log) {
  const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                      .pApplicationName = "vulpen",
                                      .apiVersion = VK_API_VERSION_1_2};
  const bool validated = validate && layer_installed(validation_layer);
  const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                  .pApplicationInfo = &application,
                                  .enabledLayerCount = validated ? 1u : 0u,
                                  .ppEnabledLayerNames = &validation_layer};
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

std::optional<std::uint32_t> compute_family(VkPhysicalDevice device) {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
  for (std::uint32_t index = 0; index < count; ++index)
    if (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT)
      return index;
  return std::nullopt;
}

std::size_t rank(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  return static_cast<std::size_t>(std::ranges::find(preference, properties.deviceType) -
                                  preference.begin());
}

VkPhysicalDevice pick_device(VkInstance instance, const Log &log) {
  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  std::erase_if(devices, [](VkPhysicalDevice device) {
    return !meets_floor(device) || !compute_family(device);
  });
  if (devices.empty())
    throw std::runtime_error("no GPU meets the Vulkan floor: 1.2 with buffer device "
                             "address and descriptor indexing (RV01)");
  const VkPhysicalDevice best = *std::ranges::min_element(devices, {}, rank);
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(best, &properties);
  log.write(Level::info, std::format("GPU: {}", properties.deviceName));
  return best;
}

VkDevice create_device(VkPhysicalDevice physical_device, std::uint32_t family) {
  const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueFamilyIndex = family,
                                      .queueCount = 1,
                                      .pQueuePriorities = &queue_priority};
  const VkPhysicalDeviceVulkan12Features features12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .descriptorIndexing = VK_TRUE,
      .bufferDeviceAddress = VK_TRUE};
  const VkDeviceCreateInfo info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                .pNext = &features12,
                                .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &queue};
  VkDevice device = VK_NULL_HANDLE;
  check(vkCreateDevice(physical_device, &info, nullptr, &device), "vkCreateDevice");
  return device;
}

} // namespace

void check(VkResult result, const char *call) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::format("{} failed: VkResult {}", call, int{result}));
}

Mechanics::Mechanics(const Log &log)
    : _instance(create_instance(log)), _physical_device(pick_device(_instance, log)),
      _queue_family(*compute_family(_physical_device)),
      _device(create_device(_physical_device, _queue_family)) {
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

void Mechanics::submit() const {
  check(vkEndCommandBuffer(_commands), "vkEndCommandBuffer");
  check(vkResetFences(_device, 1, &_fence), "vkResetFences");
  const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &_commands};
  check(vkQueueSubmit(_queue, 1, &submit, _fence), "vkQueueSubmit");
}

} // namespace VP
