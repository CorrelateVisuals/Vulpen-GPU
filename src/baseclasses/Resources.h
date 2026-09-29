#pragma once

#include "baseclasses/Mechanics.h"

#include <cstddef>
#include <span>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace VP {

// Where a buffer lives: only on the GPU, or where the CPU writes or reads it back.
enum class Memory { device, upload, readback };

// What the GPU holds: one owner for every buffer and image; the rest borrow handles.
class Buffer {
public:
  Buffer(Buffer &&other) noexcept;
  Buffer &operator=(Buffer &&other) noexcept;
  ~Buffer();

  VkBuffer handle() const;
  VkDeviceAddress address() const;
  VkDeviceSize size() const;
  Memory memory() const;
  // The CPU's view of an upload or readback buffer; empty for device memory.
  std::span<std::byte> bytes() const;
  void flush() const;
  void invalidate() const;

private:
  friend class Resources;
  Buffer() = default;

  VmaAllocator_T *_allocator = nullptr;
  VkBuffer _buffer = VK_NULL_HANDLE;
  VmaAllocation_T *_allocation = nullptr;
  VkDeviceAddress _address = 0;
  std::byte *_mapped = nullptr;
  VkDeviceSize _size = 0;
  Memory _memory = Memory::device;
};

class Resources {
public:
  explicit Resources(const Mechanics &mechanics);
  ~Resources();
  Resources(const Resources &) = delete;
  Resources &operator=(const Resources &) = delete;

  // Every buffer is reachable by its device address, the way shaders take buffers (RV02).
  Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory) const;

private:
  VkDevice _device;
  VmaAllocator_T *_allocator = nullptr;
};

class Image {};

} // namespace VP
