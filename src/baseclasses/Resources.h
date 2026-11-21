#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace VP {

// Where a buffer lives: only on the GPU, or where the CPU writes or reads it back.
enum class Memory { device, upload, readback };

// What gives an image its pixels: a copy from what the CPU filled, a draw, or a
// dispatch's stores; or the depth a draw tests and writes.
enum class Fill { copy, draw, store, depth };

// What a depth image clears to each frame: the far plane, so the first draw keeps all.
inline constexpr float far_depth = 1.0f;

// An image's mip levels as an upload lays them out in one buffer, one after another:
// level 0 the image, each next half the one before, rounded down, never below a pixel.
std::uint32_t mip_level_count(VkExtent2D extent);
VkExtent2D mip_level_extent(VkExtent2D extent, std::uint32_t level);
VkDeviceSize
mip_level_offset(VkExtent2D extent, std::uint32_t pixel_bytes, std::uint32_t level);
// Whether the CPU makes mip levels of the format: the 8-bit ones, averaged in integers,
// so every machine makes the same bits (C01).
bool makes_mip_levels(VkFormat format);
// Fills chain's levels past 0, which it holds, up to levels, as laid out above.
void make_mip_levels(std::span<std::byte> chain,
                     VkExtent2D extent,
                     VkFormat format,
                     std::uint32_t levels);

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

// An image shaders sample, in device memory, which a copy or a draw fills.
class Image {
public:
  Image(Image &&other) noexcept;
  Image &operator=(Image &&other) noexcept;
  ~Image();

  VkImage handle() const;
  VkImageView view() const;
  VkExtent2D extent() const;
  VkFormat format() const;
  Fill fill() const;
  std::uint32_t mip_levels() const;

private:
  friend class Resources;
  Image() = default;

  VmaAllocator_T *_allocator = nullptr;
  VkDevice _device = VK_NULL_HANDLE;
  VkImage _image = VK_NULL_HANDLE;
  VmaAllocation_T *_allocation = nullptr;
  VkImageView _view = VK_NULL_HANDLE;
  VkExtent2D _extent{};
  VkFormat _format = VK_FORMAT_UNDEFINED;
  Fill _fill = Fill::copy;
  std::uint32_t _mip_levels = 1;
};

class Resources {
public:
  // budget: whether the device tells what memory it has free (VK_EXT_memory_budget).
  Resources(VkInstance instance,
            VkPhysicalDevice physical_device,
            VkDevice device,
            bool budget);
  ~Resources();
  Resources(const Resources &) = delete;
  Resources &operator=(const Resources &) = delete;

  // Every buffer is reachable by its device address, the way shaders take buffers (RV02).
  // A buffer or an image is made only within the memory the GPU has free, so one it
  // cannot hold throws, naming its size and what is free, before any memory is taken.
  Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory) const;
  Image image(VkExtent2D extent,
              VkFormat format,
              Fill fill = Fill::copy,
              std::uint32_t mip_levels = 1) const;
  // Whether shaders may sample, filtered, an image of the format filled so; a draw also
  // blends into it, and a dispatch stores into it from images[].
  bool samples(VkFormat format, Fill fill = Fill::copy) const;
  // The format every depth image takes: the first of D32_SFLOAT, X8_D24_UNORM_PACK32 and
  // D16_UNORM a draw tests and a shader samples on this GPU, as Vulkan promises the last.
  VkFormat depth_format() const;
  // The most workgroups a dispatch runs along x on this GPU.
  std::uint32_t max_compute_work_group_count() const;
  // The most bytes a uniform block holds on this GPU; Vulkan guarantees 16,384.
  std::uint32_t max_uniform_buffer_range() const;
  // Whether a draw may run a shader of the stage: a geometry or a tessellation stage
  // needs a feature past the Vulkan floor, which the device enables where the GPU has it.
  bool runs(VkShaderStageFlagBits stage) const;

private:
  const VkPhysicalDevice _physical_device;
  const VkDevice _device;
  VmaAllocator_T *_allocator = nullptr;
  VkFormat _depth_format = VK_FORMAT_UNDEFINED;
};

} // namespace VP
