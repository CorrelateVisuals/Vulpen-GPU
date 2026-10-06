#include "baseclasses/Resources.h"

#include "baseclasses/Mechanics.h"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <utility>

namespace VP {

namespace {

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

Resources::Resources(VkInstance instance,
                     VkPhysicalDevice physical_device,
                     VkDevice device)
    : _physical_device(physical_device), _device(device) {
  const VmaAllocatorCreateInfo info{.flags =
                                        VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
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
  const VmaAllocationCreateInfo where{.flags = placement(memory),
                                      .usage = VMA_MEMORY_USAGE_AUTO};
  Buffer buffer;
  VmaAllocationInfo allocated{};
  check(vmaCreateBuffer(
            _allocator, &info, &where, &buffer._buffer, &buffer._allocation, &allocated),
        "vmaCreateBuffer");
  buffer._allocator = _allocator;
  buffer._mapped = static_cast<std::byte *>(allocated.pMappedData);
  buffer._size = size;
  buffer._memory = memory;
  const VkBufferDeviceAddressInfo address{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer._buffer};
  buffer._address = vkGetBufferDeviceAddress(_device, &address);
  return buffer;
}

Image Resources::image(VkExtent2D extent, VkFormat format) const {
  const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = format,
                               .extent = {extent.width, extent.height, 1},
                               .mipLevels = 1,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT};
  const VmaAllocationCreateInfo where{.usage = VMA_MEMORY_USAGE_AUTO};
  Image image;
  check(vmaCreateImage(
            _allocator, &info, &where, &image._image, &image._allocation, nullptr),
        "vmaCreateImage");
  image._allocator = _allocator;
  image._device = _device;
  image._extent = extent;
  image._format = format;
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
// 32-bit floats, and a linear sampler there reads what each vendor likes (GLSL02).
bool Resources::samples(VkFormat format) const {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(_physical_device, format, &properties);
  const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return (properties.optimalTilingFeatures & needed) == needed;
}

} // namespace VP
