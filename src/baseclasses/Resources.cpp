#include "baseclasses/Resources.h"

#include "baseclasses/Mechanics.h"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace VP {

namespace {

constexpr double bytes_a_gigabyte = 1e9; // as a person reads a size

// Every allocation stays within what the GPU has free, so one it cannot hold fails before
// any memory is taken, rather than taking it from the machine's own (A03).
constexpr VmaAllocationCreateFlags within = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;

// What the heap of a memory type has free: its budget less what this process uses of it.
VkDeviceSize free_of(VmaAllocator allocator, std::uint32_t type) {
  const VkPhysicalDeviceMemoryProperties *memory = nullptr;
  vmaGetMemoryProperties(allocator, &memory);
  std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
  vmaGetHeapBudgets(allocator, budgets.data());
  const VmaBudget &heap = budgets[memory->memoryTypes[type].heapIndex];
  return heap.budget > heap.usage ? heap.budget - heap.usage : 0;
}

std::string gigabytes(VkDeviceSize bytes) {
  return std::format("{:.2f} GB", static_cast<double>(bytes) / bytes_a_gigabyte);
}

// Throws naming what an allocation that failed asked for and what is free, as a count
// the GPU cannot hold fails so (A02); any other failure as check does.
void allocated(VkResult result,
               const char *call,
               std::string_view asked,
               VkDeviceSize free) {
  if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)
    throw std::runtime_error(std::format(
        "{} is more than the GPU can give, with {} free", asked, gigabytes(free)));
  check(result, call);
}

// A side past what the GPU makes is refused, naming both, rather than made against the
// spec, which one driver takes and another does not (A02, RVK00); a draw renders
// through a framebuffer, whose sides may be shorter still.
void fits(VkPhysicalDevice device, VkExtent2D extent, Fill fill) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  const VkPhysicalDeviceLimits &limits = properties.limits;
  const bool drawn = fill == Fill::draw || fill == Fill::depth;
  const std::uint32_t widest =
      drawn ? std::min(limits.maxImageDimension2D, limits.maxFramebufferWidth)
            : limits.maxImageDimension2D;
  const std::uint32_t tallest =
      drawn ? std::min(limits.maxImageDimension2D, limits.maxFramebufferHeight)
            : limits.maxImageDimension2D;
  if (extent.width > widest || extent.height > tallest)
    throw std::runtime_error(
        std::format("an image of {} by {} pixels is past the {} by {} this GPU makes",
                    extent.width,
                    extent.height,
                    widest,
                    tallest));
}

VmaAllocationCreateFlags placement(Memory memory) {
  switch (memory) {
    case Memory::upload:
      return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
             VMA_ALLOCATION_CREATE_MAPPED_BIT;
    case Memory::readback:
      return VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
             VMA_ALLOCATION_CREATE_MAPPED_BIT;
    case Memory::device:
      break;
  }
  return 0;
}

} // namespace

Buffer::Buffer(Buffer &&other) noexcept {
  *this = std::move(other);
}

Buffer &Buffer::operator=(Buffer &&other) noexcept {
  std::swap(_allocator, other._allocator);
  std::swap(_buffer, other._buffer);
  std::swap(_allocation, other._allocation);
  std::swap(_address, other._address);
  std::swap(_mapped, other._mapped);
  std::swap(_size, other._size);
  std::swap(_memory, other._memory);
  return *this;
}

Buffer::~Buffer() {
  if (_buffer)
    vmaDestroyBuffer(_allocator, _buffer, _allocation);
}

VkBuffer Buffer::handle() const {
  return _buffer;
}

VkDeviceAddress Buffer::address() const {
  return _address;
}

VkDeviceSize Buffer::size() const {
  return _size;
}

Memory Buffer::memory() const {
  return _memory;
}

std::span<std::byte> Buffer::bytes() const {
  if (!_mapped)
    return {};
  return {_mapped, _size};
}

void Buffer::flush() const {
  check(vmaFlushAllocation(_allocator, _allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");
}

void Buffer::invalidate() const {
  check(vmaInvalidateAllocation(_allocator, _allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
}

Image::Image(Image &&other) noexcept {
  *this = std::move(other);
}

Image &Image::operator=(Image &&other) noexcept {
  std::swap(_allocator, other._allocator);
  std::swap(_device, other._device);
  std::swap(_image, other._image);
  std::swap(_allocation, other._allocation);
  std::swap(_view, other._view);
  std::swap(_extent, other._extent);
  std::swap(_format, other._format);
  std::swap(_fill, other._fill);
  std::swap(_mip_levels, other._mip_levels);
  return *this;
}

Image::~Image() {
  if (!_image)
    return;
  vkDestroyImageView(_device, _view, nullptr);
  vmaDestroyImage(_allocator, _image, _allocation);
}

VkImage Image::handle() const {
  return _image;
}

VkImageView Image::view() const {
  return _view;
}

VkExtent2D Image::extent() const {
  return _extent;
}

VkFormat Image::format() const {
  return _format;
}

Fill Image::fill() const {
  return _fill;
}

std::uint32_t Image::mip_levels() const {
  return _mip_levels;
}

Resources::Resources(VkInstance instance,
                     VkPhysicalDevice physical_device,
                     VkDevice device,
                     bool budget)
    : _physical_device(physical_device), _device(device) {
  const VmaAllocatorCreateFlags told =
      budget ? VmaAllocatorCreateFlags{VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT} : 0;
  const VmaAllocatorCreateInfo info{
      .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | told,
      .physicalDevice = physical_device,
      .device = device,
      .instance = instance,
      .vulkanApiVersion = VK_API_VERSION_1_2};
  check(vmaCreateAllocator(&info, &_allocator), "vmaCreateAllocator");
  constexpr std::array depths{
      VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM};
  constexpr VkFormatFeatureFlags tested = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  for (const VkFormat format : depths) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    if ((properties.optimalTilingFeatures & tested) == tested) {
      _depth_format = format;
      break;
    }
  }
}

Resources::~Resources() {
  vmaDestroyAllocator(_allocator);
}

Buffer
Resources::buffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory) const {
  const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = size,
                                .usage =
                                    usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
  const VmaAllocationCreateInfo where{.flags = placement(memory) | within,
                                      .usage = VMA_MEMORY_USAGE_AUTO};
  Buffer buffer;
  VmaAllocationInfo made{};
  if (const VkResult result = vmaCreateBuffer(
          _allocator, &info, &where, &buffer._buffer, &buffer._allocation, &made);
      result != VK_SUCCESS) {
    // The type such a buffer takes, found for one of a byte, as one of the size that
    // failed would fail again.
    VkBufferCreateInfo least = info;
    least.size = 1;
    std::uint32_t type = 0;
    const bool typed = vmaFindMemoryTypeIndexForBufferInfo(
                           _allocator, &least, &where, &type) == VK_SUCCESS;
    allocated(result,
              "vmaCreateBuffer",
              gigabytes(size),
              typed ? free_of(_allocator, type) : 0);
  }
  buffer._allocator = _allocator;
  buffer._mapped = static_cast<std::byte *>(made.pMappedData);
  buffer._size = size;
  buffer._memory = memory;
  const VkBufferDeviceAddressInfo address{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer._buffer};
  buffer._address = vkGetBufferDeviceAddress(_device, &address);
  return buffer;
}

// A stored image is zeroed by a transfer once made.
Image Resources::image(VkExtent2D extent,
                       VkFormat format,
                       Fill fill,
                       std::uint32_t mip_levels) const {
  fits(_physical_device, extent, fill);
  const VkImageUsageFlags filled =
      fill == Fill::draw    ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      : fill == Fill::store ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      : fill == Fill::depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = format,
                               .extent = {extent.width, extent.height, 1},
                               .mipLevels = mip_levels,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_SAMPLED_BIT | filled};
  const VmaAllocationCreateInfo where{.flags = within, .usage = VMA_MEMORY_USAGE_AUTO};
  Image image;
  if (const VkResult result = vmaCreateImage(
          _allocator, &info, &where, &image._image, &image._allocation, nullptr);
      result != VK_SUCCESS) {
    VkImageCreateInfo least = info; // as a buffer's, for a pixel
    least.extent = {1, 1, 1};
    std::uint32_t type = 0;
    const bool typed = vmaFindMemoryTypeIndexForImageInfo(
                           _allocator, &least, &where, &type) == VK_SUCCESS;
    allocated(result,
              "vmaCreateImage",
              std::format("an image of {} by {} pixels", extent.width, extent.height),
              typed ? free_of(_allocator, type) : 0);
  }
  image._allocator = _allocator;
  image._device = _device;
  image._extent = extent;
  image._format = format;
  image._fill = fill;
  image._mip_levels = mip_levels;
  const VkImageViewCreateInfo view{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image._image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {.aspectMask = fill == Fill::depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                             : VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = mip_levels,
                           .layerCount = 1}};
  check(vkCreateImageView(_device, &view, nullptr, &image._view), "vkCreateImageView");
  return image;
}

// Every format an image may take can be sampled on most GPUs, but some cannot filter
// 32-bit floats, and a linear sampler there reads what each vendor likes (GLSL02). Every
// draw blends, and some GPUs cannot blend into 32-bit floats either. A store indexes
// images[], which some GPUs cannot, and which the device then does not enable.
bool Resources::samples(VkFormat format, Fill fill) const {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(_physical_device, format, &properties);
  const VkFormatFeatureFlags needed =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      (fill == Fill::draw ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
       : fill == Fill::store
           ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT
           : VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(_physical_device, &features);
  return (properties.optimalTilingFeatures & needed) == needed &&
         (fill != Fill::store || features.shaderStorageImageArrayDynamicIndexing);
}

VkFormat Resources::depth_format() const {
  return _depth_format;
}

std::uint32_t Resources::max_compute_work_group_count() const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(_physical_device, &properties);
  return properties.limits.maxComputeWorkGroupCount[0];
}

std::uint32_t Resources::max_uniform_buffer_range() const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(_physical_device, &properties);
  return properties.limits.maxUniformBufferRange;
}

bool Resources::runs(VkShaderStageFlagBits stage) const {
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(_physical_device, &features);
  if (stage == VK_SHADER_STAGE_GEOMETRY_BIT)
    return features.geometryShader == VK_TRUE;
  if (stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ||
      stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
    return features.tessellationShader == VK_TRUE;
  return true;
}

namespace {

constexpr std::uint32_t pixel_max = 255;    // an 8-bit channel at its brightest
constexpr std::uint32_t linear_max = 65535; // linear light, held in 16 bits
constexpr std::uint64_t light_step = linear_max / pixel_max; // 8 bits to 16, exactly
constexpr std::uint32_t box = 2; // texels a level's texel averages each way
constexpr std::uint32_t rgba = 4;
constexpr std::uint32_t alpha = 3;

// sRGB's decode (IEC 61966-2-1) of each 8-bit value, as 16-bit linear light. The
// compiler works it out with + - * / alone, which IEEE 754 rounds alike everywhere
// (RC06), so every machine holds the same table (C01), where a library's pow may differ:
// x^2.4 as x^2 times the fifth root of x^2, by Newton's method from above.
constexpr double srgb_toe = 0.04045;
constexpr double srgb_toe_slope = 12.92;
constexpr double srgb_offset = 0.055;
constexpr double root_degree = 5.0;
constexpr int root_steps = 40;

constexpr double fifth_root(double value) {
  double root = 1.0;
  for (int step = 0; step < root_steps; ++step)
    root =
        ((root_degree - 1.0) * root + value / (root * root * root * root)) / root_degree;
  return root;
}

constexpr std::array<std::uint16_t, pixel_max + 1> srgb_decoded = [] {
  std::array<std::uint16_t, pixel_max + 1> table{};
  for (std::uint32_t value = 0; value <= pixel_max; ++value) {
    const double s = static_cast<double>(value) / pixel_max;
    const double x = (s + srgb_offset) / (1.0 + srgb_offset);
    const double light = s <= srgb_toe ? s / srgb_toe_slope : x * x * fifth_root(x * x);
    table[value] = static_cast<std::uint16_t>(light * linear_max + 0.5);
  }
  return table;
}();

// The 8-bit value whose decode lies nearest the light; the darker on a tie.
std::uint64_t srgb_encoded(std::uint64_t light) {
  const auto above = std::ranges::lower_bound(srgb_decoded, light);
  if (above == srgb_decoded.begin())
    return 0;
  if (above == srgb_decoded.end())
    return pixel_max;
  const auto below = std::prev(above);
  return static_cast<std::uint64_t>((light - *below <= *above - light ? below : above) -
                                    srgb_decoded.begin());
}

// An 8-bit format's channels; none for any other format.
std::uint32_t mip_channels(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
      return 1;
    case VK_FORMAT_R8G8_UNORM:
      return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
      return rgba;
    default:
      return 0;
  }
}

// The texel of the next level at x, y, from the box of 2 by 2 texels of the level above
// over it, which an odd side's last row or column falls out of as the level halves. A
// color weighs by its alpha, where the format has one, so a transparent texel lends it
// none, as a premultiplied average would; with nothing covered, the colors' plain
// average. sRGB colors average in linear light, as the sampler filters them.
void average(std::span<const std::byte> from,
             VkExtent2D above,
             std::uint32_t x,
             std::uint32_t y,
             std::uint32_t channels,
             bool srgb,
             std::span<std::byte> texel) {
  const std::uint32_t colors = channels == rgba ? alpha : channels;
  std::array<std::uint64_t, rgba> weighed{};
  std::array<std::uint64_t, rgba> plain{};
  std::uint64_t weight = 0;
  for (std::uint32_t dy = 0; dy < box; ++dy)
    for (std::uint32_t dx = 0; dx < box; ++dx) {
      const std::uint32_t column = std::min(x * box + dx, above.width - 1);
      const std::uint32_t row = std::min(y * box + dy, above.height - 1);
      const std::span<const std::byte> under =
          from.subspan((std::size_t{row} * above.width + column) * channels, channels);
      const std::uint64_t covers =
          channels == rgba ? std::to_integer<std::uint64_t>(under[alpha]) : 1;
      weight += covers;
      for (std::uint32_t c = 0; c < channels; ++c) {
        const auto value = std::to_integer<std::uint64_t>(under[c]);
        const std::uint64_t light =
            srgb && c < colors ? srgb_decoded[value] : value * light_step;
        weighed[c] += light * covers;
        plain[c] += light;
      }
    }
  constexpr std::uint64_t texels = box * box;
  for (std::uint32_t c = 0; c < channels; ++c) {
    const std::uint64_t light = c < colors && weight > 0
                                    ? (weighed[c] + weight / 2) / weight
                                    : (plain[c] + texels / 2) / texels;
    texel[c] = static_cast<std::byte>(srgb && c < colors
                                          ? srgb_encoded(light)
                                          : (2 * light + light_step) / (2 * light_step));
  }
}

} // namespace

std::uint32_t mip_level_count(VkExtent2D extent) {
  return static_cast<std::uint32_t>(
      std::bit_width(std::max(extent.width, extent.height)));
}

VkExtent2D mip_level_extent(VkExtent2D extent, std::uint32_t level) {
  return {std::max(extent.width >> level, 1U), std::max(extent.height >> level, 1U)};
}

VkDeviceSize
mip_level_offset(VkExtent2D extent, std::uint32_t pixel_bytes, std::uint32_t level) {
  VkDeviceSize offset = 0;
  for (std::uint32_t above = 0; above < level; ++above) {
    const VkExtent2D size = mip_level_extent(extent, above);
    offset += VkDeviceSize{size.width} * size.height * pixel_bytes;
  }
  return offset;
}

bool makes_mip_levels(VkFormat format) {
  return mip_channels(format) != 0;
}

void make_mip_levels(std::span<std::byte> chain,
                     VkExtent2D extent,
                     VkFormat format,
                     std::uint32_t levels) {
  const std::uint32_t channels = mip_channels(format);
  const bool srgb = format == VK_FORMAT_R8G8B8A8_SRGB;
  for (std::uint32_t level = 1; level < levels; ++level) {
    const VkExtent2D above = mip_level_extent(extent, level - 1);
    const VkExtent2D below = mip_level_extent(extent, level);
    const std::span<const std::byte> from =
        chain.subspan(mip_level_offset(extent, channels, level - 1));
    const std::span<std::byte> to =
        chain.subspan(mip_level_offset(extent, channels, level));
    for (std::uint32_t y = 0; y < below.height; ++y)
      for (std::uint32_t x = 0; x < below.width; ++x)
        average(from,
                above,
                x,
                y,
                channels,
                srgb,
                to.subspan((std::size_t{y} * below.width + x) * channels, channels));
  }
}

} // namespace VP
