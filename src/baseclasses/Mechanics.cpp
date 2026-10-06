#include "baseclasses/Mechanics.h"

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
#include <vector>

namespace VP {

namespace {

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
  std::uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
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
  std::uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> offered(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, offered.data());
  std::vector<const char *> extensions;
  for (const VkExtensionProperties &extension : offered)
    if (std::string_view(extension.extensionName).ends_with(surface_extension))
      extensions.push_back(extension.extensionName);
  const VkInstanceCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
      .enabledLayerCount = validated ? 1u : 0u,
      .ppEnabledLayerNames = &validation_layer,
      .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
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

bool has_swapchain(VkPhysicalDevice device) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
  return std::ranges::any_of(extensions, [](const VkExtensionProperties &extension) {
    return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
  });
}

std::vector<VkQueueFamilyProperties> families_of(VkPhysicalDevice device) {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
  return families;
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

// As the log names a device: "name: type, Vulkan 1.3.280".
std::string describe(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  const auto type = static_cast<std::size_t>(properties.deviceType);
  return std::format("{}: {}, Vulkan {}.{}.{}",
                     properties.deviceName,
                     type < device_types.size() ? device_types[type] : device_types[0],
                     VK_API_VERSION_MAJOR(properties.apiVersion),
                     VK_API_VERSION_MINOR(properties.apiVersion),
                     VK_API_VERSION_PATCH(properties.apiVersion));
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
    const char *const reason = unfit(device, surface);
    log.write(
        Level::debug,
        Tag::gpu,
        std::format("{}; {}", describe(device), reason ? reason : "meets the floor"));
    return reason != nullptr;
  });
  if (devices.empty())
    throw std::runtime_error(
        std::format("no GPU meets the Vulkan floor: 1.2 with buffer device address and "
                    "descriptor indexing (RV01){}; --log debug says why for each GPU",
                    surface ? ", and presents to the window" : ""));
  const VkPhysicalDevice best = *std::ranges::min_element(devices, {}, rank);
  log.write(Level::info, Tag::gpu, "runs on " + describe(best));
  return best;
}

VkDevice create_device(VkPhysicalDevice physical_device, std::uint32_t family) {
  const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                      .queueFamilyIndex = family,
                                      .queueCount = 1,
                                      .pQueuePriorities = &queue_priority};
  // What set 0's array of images needs (RV02), all of which descriptor indexing brings.
  const VkPhysicalDeviceFeatures features{.shaderSampledImageArrayDynamicIndexing =
                                              VK_TRUE};
  const VkPhysicalDeviceVulkan12Features features12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .descriptorIndexing = VK_TRUE,
      .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
      .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
      .descriptorBindingPartiallyBound = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
      .bufferDeviceAddress = VK_TRUE};
  const char *const swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  const VkDeviceCreateInfo info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                .pNext = &features12,
                                .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &queue,
                                .enabledExtensionCount =
                                    has_swapchain(physical_device) ? 1u : 0u,
                                .ppEnabledExtensionNames = &swapchain,
                                .pEnabledFeatures = &features};
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
    : _instance(create_instance(log)), _choice(choose(_instance, window, log)),
      _device(create_device(_choice.device, _choice.family)) {
  vkGetDeviceQueue(_device, _choice.family, 0, &_queue);
  const VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .flags =
                                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                     .queueFamilyIndex = _choice.family};
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
  return _choice.device;
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

// A window at start gets a device and queue that present to it, through a surface made
// only for the choice: the window's output makes its own.
Mechanics::Choice
Mechanics::choose(VkInstance instance, const Window *window, const Log &log) {
  const VkSurfaceKHR surface = window ? window->surface(instance) : VK_NULL_HANDLE;
  const VkPhysicalDevice device = pick_device(instance, surface, log);
  const Choice choice{.device = device, .family = *queue_family(device, surface)};
  if (surface)
    vkDestroySurfaceKHR(instance, surface, nullptr);
  return choice;
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
