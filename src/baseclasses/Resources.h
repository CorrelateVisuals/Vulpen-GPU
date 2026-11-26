#pragma once

#include "baseclasses/Owned.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace VP {

// "All things were made by him; and without him was not any thing made that was made."
// John 1:3

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

// All a buffer is, which goes and moves as one: VMA's handle with the allocator and
// memory it came from, and what the buffer is besides, so one moved from holds none of
// it.
struct BufferMade {
  VmaAllocator_T *allocator = nullptr;
  VkBuffer handle = VK_NULL_HANDLE;
  VmaAllocation_T *allocation = nullptr;
  VkDeviceAddress address = 0;
  std::byte *mapped = nullptr;
  VkDeviceSize size = 0;
  Memory memory = Memory::device;
  bool operator==(const BufferMade &) const = default;
};
// As BufferMade, for an image; its view is apart, as it goes before the image.
struct ImageMade {
  VmaAllocator_T *allocator = nullptr;
  VkImage handle = VK_NULL_HANDLE;
  VmaAllocation_T *allocation = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  Fill fill = Fill::copy;
  std::uint32_t mip_levels = 1;
  bool operator==(const ImageMade &) const = default;
};
// VMA's destroys, as an Owned takes them: VMA's header stays in Resources.cpp.
void destroy_buffer(BufferMade buffer);
void destroy_image(ImageMade image);
void destroy_allocator(VmaAllocator_T *allocator);

// What the GPU holds: one owner for every buffer and image; the rest borrow handles.
class Buffer {
public:
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

  Owned<BufferMade, destroy_buffer> _made;
};

// An image shaders sample, in device memory, which a copy or a draw fills.
class Image {
public:
  VkImage handle() const;
  VkImageView view() const;
  VkExtent2D extent() const;
  VkFormat format() const;
  Fill fill() const;
  std::uint32_t mip_levels() const;

private:
  friend class Resources;
  Image() = default;

  Owned<ImageMade, destroy_image> _made;
  // Declared after, so it goes first.
  Owned<VkImageView, vkDestroyImageView, VkDevice> _view;
};

class Resources {
public:
  // budget: whether the device tells what memory it has free (VK_EXT_memory_budget).
  Resources(VkInstance instance,
            VkPhysicalDevice physical_device,
            VkDevice device,
            bool budget);
  Resources(const Resources &) = delete;
  Resources &operator=(const Resources &) = delete;

  // Every buffer is reachable by its device address, the way shaders take buffers (RV02).
  // A buffer or an image is made only within the memory the GPU has free, so one it
  // cannot hold throws, naming its size and what is free, before any memory is taken.
  Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory) const;
  // With samples past one, a draw's color or depth that the render pass resolves or
  // drops, which no shader samples; in lazily allocated memory where the GPU has it, so
  // a tile-based GPU may keep it on the chip and back it with none.
  Image image(VkExtent2D extent,
              VkFormat format,
              Fill fill = Fill::copy,
              std::uint32_t mip_levels = 1,
              VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) const;
  // Whether shaders may sample, filtered, an image of the format filled so; a draw also
  // blends into it, and a dispatch stores into it from images[]. A depth is sampled
  // nearest, and a draw tests and writes it.
  bool samples(VkFormat format, Fill fill = Fill::copy) const;
  // The format every depth image takes: the first of D32_SFLOAT, X8_D24_UNORM_PACK32 and
  // D16_UNORM a draw tests and a shader samples on this GPU, as Vulkan promises the last.
  VkFormat depth_format() const;
  // The samples a pixel this GPU draws with into both a color and a depth; Vulkan
  // promises 1 and 4.
  VkSampleCountFlags sample_counts() const;
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
  Owned<VmaAllocator_T *, destroy_allocator> _allocator;
  VkFormat _depth_format = VK_FORMAT_UNDEFINED;
};

} // namespace VP
