#include "baseclasses/Resources.h"

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

Resources::Resources(const Mechanics &mechanics) : _device(mechanics.device()) {
  const VmaAllocatorCreateInfo info{.flags =
                                        VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
                                    .physicalDevice = mechanics.physical_device(),
                                    .device = mechanics.device(),
                                    .instance = mechanics.instance(),
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

} // namespace VP
