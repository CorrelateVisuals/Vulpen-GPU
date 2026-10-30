#include "baseclasses/Resources.h"

#include "baseclasses/Mechanics.h"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <array>
#include <format>
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
void allocated(VkResult result, const char *call, std::string_view asked, VkDeviceSize free) {
  if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)
    throw std::runtime_error(std::format(
        "{} is more than the GPU can give, with {} free", asked, gigabytes(free)));
  check(result, call);
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

Resources::Resources(VkInstance instance,
                     VkPhysicalDevice physical_device,
                     VkDevice device,
                     bool budget)
    : _physical_device(physical_device), _device(device) {
  const VmaAllocatorCreateFlags told =
      budget ? VmaAllocatorCreateFlags{VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT} : 0;
  const VmaAllocatorCreateInfo info{.flags =
                                        VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | told,
                                    .physicalDevice = physical_device,
                                    .device = device,
                                    .instance = instance,
                                    .vulkanApiVersion = VK_API_VERSION_1_2};
  check(vmaCreateAllocator(&info, &_allocator), "vmaCreateAllocator");
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
    const bool typed =
        vmaFindMemoryTypeIndexForBufferInfo(_allocator, &least, &where, &type) == VK_SUCCESS;
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
Image Resources::image(VkExtent2D extent, VkFormat format, Fill fill) const {
  const VkImageUsageFlags filled =
      fill == Fill::draw    ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      : fill == Fill::store ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = format,
                               .extent = {extent.width, extent.height, 1},
                               .mipLevels = 1,
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
    const bool typed =
        vmaFindMemoryTypeIndexForImageInfo(_allocator, &least, &where, &type) == VK_SUCCESS;
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
  const VkImageViewCreateInfo view{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image._image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
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
      (fill == Fill::draw    ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                   VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
       : fill == Fill::store ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT
                             : VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(_physical_device, &features);
  return (properties.optimalTilingFeatures & needed) == needed &&
         (fill != Fill::store || features.shaderStorageImageArrayDynamicIndexing);
}

std::uint32_t Resources::max_compute_work_group_count() const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(_physical_device, &properties);
  return properties.limits.maxComputeWorkGroupCount[0];
}

} // namespace VP
