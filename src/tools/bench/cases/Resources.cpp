#include "baseclasses/Resources.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// baseclasses/Resources.cpp: the GPU's buffers and images, on a device on lavapipe
// (bench/Gpu.h). A buffer or an image a row makes goes with the row, so the last row, as
// the device goes, shows anything left (A01). What lavapipe has free follows the
// machine's memory, so a refusal shows it as <free>; sizes stay either small or past any
// machine's, as one between would take the machine's own memory.

namespace VP::bench {

namespace domain {
// A buffer's size: elements times their stride, as the schedule asks, and past it.
struct Size {};
// A buffer's usage, as the engine's callers pass it, and none.
struct Usage {};
// An image's format: every one Gpu.h names but none, which no caller passes, as
// samples() refuses it, and on which the layer's synchronization validation crashes.
struct ImageFormat {};
} // namespace domain

template <> struct Gen<domain::Size> {
  using type = VkDeviceSize;
  static std::vector<VkDeviceSize> edges() {
    constexpr VkDeviceSize most = std::numeric_limits<VkDeviceSize>::max();
    return {256,
            1,
            0,
            3,
            4,
            16,
            65536,
            VkDeviceSize{1} << 20,
            VkDeviceSize{1} << 24,
            VkDeviceSize{1} << 40,
            VkDeviceSize{1} << 62,
            most - 255,
            most};
  }
  static VkDeviceSize random(Random &random) {
    constexpr VkDeviceSize small = VkDeviceSize{1} << 20;
    constexpr VkDeviceSize past = VkDeviceSize{1} << 40; // past any machine's memory
    return random.below(2) == 0 ? 1 + random.below(small) : past + random.below(past);
  }
};

template <> struct Gen<domain::Usage> {
  using type = VkBufferUsageFlags;
  static std::vector<VkBufferUsageFlags> edges() {
    return {VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            0,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  }
  static VkBufferUsageFlags random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::ImageFormat> {
  using type = VkFormat;
  static std::vector<VkFormat> edges() {
    std::vector<VkFormat> all = Gen<VkFormat>::edges();
    std::erase(all, VK_FORMAT_UNDEFINED);
    return all;
  }
  static VkFormat random(Random &random) {
    const std::vector<VkFormat> all = edges();
    return all[random.below(all.size())];
  }
};

template <> struct Gen<VkShaderStageFlagBits> {
  using type = VkShaderStageFlagBits;
  static std::vector<VkShaderStageFlagBits> edges() {
    return {VK_SHADER_STAGE_VERTEX_BIT,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_SHADER_STAGE_GEOMETRY_BIT,
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
            VK_SHADER_STAGE_COMPUTE_BIT,
            VK_SHADER_STAGE_ALL_GRAPHICS};
  }
  static VkShaderStageFlagBits random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<VkResult> {
  using type = VkResult;
  static std::vector<VkResult> edges() {
    return {VK_ERROR_OUT_OF_DEVICE_MEMORY,
            VK_SUCCESS,
            VK_ERROR_OUT_OF_HOST_MEMORY,
            VK_ERROR_INITIALIZATION_FAILED,
            VK_ERROR_DEVICE_LOST,
            VK_ERROR_FORMAT_NOT_SUPPORTED,
            VK_INCOMPLETE};
  }
  static VkResult random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

// Its handle, size and memory, how much of it the CPU sees, and whether its address
// meets std430's vec4 alignment, which a shader's buffer_reference takes as given; the
// address itself follows where the driver put it.
template <> struct Shown<Buffer> {
  static std::string text(const Buffer &buffer, Names &names) {
    constexpr VkDeviceAddress vec4_alignment = 16;
    return std::format("buffer {} of {} bytes {}, {} mapped, address {}",
                       show(buffer.handle(), names),
                       buffer.size(),
                       show(buffer.memory(), names),
                       buffer.bytes().size(),
                       buffer.address() == 0                    ? "none"
                       : buffer.address() % vec4_alignment == 0 ? "16-aligned"
                                                                : "not 16-aligned");
  }
};

template <> struct Shown<Image> {
  static std::string text(const Image &image, Names &names) {
    return std::format("image {} {} {} {}, view {}",
                       show(image.handle(), names),
                       show(image.extent(), names),
                       show(image.format(), names),
                       show(image.fill(), names),
                       show(image.view(), names));
  }
};

namespace {

// What lavapipe has free follows the machine's memory: "with 12.34 GB free" shows as
// "with <free> GB free". None free shows as it is, as a refusal that found no memory
// type for the allocation says.
std::string free_hidden(std::string text) {
  constexpr std::string_view before = "with ";
  constexpr std::string_view after = " GB free";
  const std::size_t end = text.find(after);
  const std::size_t start = end == std::string::npos ? end : text.rfind(before, end);
  const std::size_t from = start + before.size();
  if (start != std::string::npos && text.compare(from, end - from, "0.00") != 0)
    text.replace(from, end - from, "<free>");
  return text;
}

// A row's result as its text stands.
struct Said {
  std::string text;
};

// A call's result, or what it threw, with the free memory hidden.
template <class F> Said made(F &&call, Names &names) {
  try {
    return {show(call(), names)};
  } catch (...) {
    return {free_hidden(thrown())};
  }
}

} // namespace

template <> struct Shown<Said> {
  static std::string text(const Said &said, Names &) {
    return said.text;
  }
};

void cases(Bench &bench) {
  bench.sweep("Resources.cpp/gigabytes", gigabytes);
  bench.sweep("Resources.cpp/allocated", [](VkResult result, VkDeviceSize free) {
    VP::allocated(result, "vmaCreateBuffer", "1.00 GB", free);
  });
  bench.sweep("Resources.cpp/placement", placement);

  bench.check("Resources.cpp/fits", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(
          rows, "Resources.cpp/fits", gpu, [](Gpu &on, VkExtent2D extent, Fill fill) {
            fits(on.mechanics.physical_device(), extent, fill);
          });
    });
  });

  bench.check("Resources.cpp/Resources::buffer", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(
          rows,
          "Resources.cpp/Resources::buffer",
          gpu,
          [&rows](Gpu &on, VkDeviceSize size, VkBufferUsageFlags usage, Memory memory) {
            return made([&] { return on.resources.buffer(size, usage, memory); },
                        rows.names());
          },
          domain::Size{},
          domain::Usage{});
    });
  });

  bench.check("Resources.cpp/Resources::image", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(
          rows,
          "Resources.cpp/Resources::image",
          gpu,
          [&rows](Gpu &on, VkExtent2D extent, VkFormat format, Fill fill) {
            return made([&] { return on.resources.image(extent, format, fill); },
                        rows.names());
          },
          domain::Type{},
          domain::ImageFormat{});
    });
  });

  bench.check("Resources.cpp/Resources::samples", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(rows,
               "Resources.cpp/Resources::samples",
               gpu,
               [](Gpu &on, VkFormat format, Fill fill) {
                 return on.resources.samples(format, fill);
               });
    });
  });

  // The claim callers lean on: a format samples() takes for a fill makes an image of it,
  // with no word from the layer.
  bench.check("Resources.cpp/Resources::samples", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      for (const VkFormat format : Gen<VkFormat>::edges())
        for (const Fill fill : Gen<Fill>::edges())
          if (gpu.resources.samples(format, fill))
            rows.row(std::format(
                "taken {} {} -> {}",
                show(format, rows.names()),
                show(fill, rows.names()),
                made([&] { return gpu.resources.image({64, 64}, format, fill); },
                     rows.names())
                    .text));
    });
  });

  bench.check("Resources.cpp/Resources::runs", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(
          rows,
          "Resources.cpp/Resources::runs",
          gpu,
          [](Gpu &on, VkShaderStageFlagBits stage) { return on.resources.runs(stage); });
      rows.row(std::format("max_compute_work_group_count() -> {}",
                           gpu.resources.max_compute_work_group_count()));
    });
  });

  // One owner, through every move: a buffer moved from holds nothing, or what the one
  // moved onto held, which goes with it (A01, CPP09).
  bench.check("Resources.cpp/Buffer", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const auto made_buffer = [&](Memory memory) {
        return gpu.resources.buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory);
      };
      Names &names = rows.names();
      {
        Buffer first = made_buffer(Memory::upload);
        const VkBuffer held = first.handle();
        Buffer second(std::move(first));
        rows.row(std::format("moved into a new one: it holds {}, the old {}",
                             show(second.handle() == held, names),
                             show(first, names)));
      }
      {
        Buffer first = made_buffer(Memory::upload);
        Buffer second = made_buffer(Memory::readback);
        const VkBuffer was = second.handle();
        second = std::move(first);
        rows.row(std::format("moved onto one that holds a buffer: the old one holds what "
                             "it held: {}",
                             show(first.handle() == was, names)));
      }
      {
        Buffer first = made_buffer(Memory::device);
        Buffer &same = first;
        first = std::move(same);
        rows.row("moved onto itself -> " + show(first, names));
      }
      for (const Memory memory : {Memory::upload, Memory::readback}) {
        const Buffer buffer = made_buffer(memory);
        for (std::size_t index = 0; index < buffer.bytes().size(); ++index)
          buffer.bytes()[index] = static_cast<std::byte>(index * 7);
        buffer.flush();
        buffer.invalidate();
        bool kept = true;
        for (std::size_t index = 0; index < buffer.bytes().size(); ++index)
          kept = kept && buffer.bytes()[index] == static_cast<std::byte>(index * 7);
        rows.row(std::format("{} bytes written, flushed, invalidated: read back {}",
                             show(memory, names),
                             show(kept, names)));
      }
      const Buffer device = made_buffer(Memory::device);
      rows.row(std::format("device memory: flush and invalidate -> {}", [&] {
        try {
          device.flush();
          device.invalidate();
          return std::string("done");
        } catch (...) {
          return thrown();
        }
      }()));
    });
  });

  bench.check("Resources.cpp/Image", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      Names &names = rows.names();
      {
        Image first = gpu.resources.image({8, 8}, VK_FORMAT_R8G8B8A8_UNORM);
        const VkImage held = first.handle();
        Image second(std::move(first));
        rows.row(std::format("moved into a new one: it holds {}, the old {}",
                             show(second.handle() == held, names),
                             show(first, names)));
      }
      {
        Image first = gpu.resources.image({8, 8}, VK_FORMAT_R8G8B8A8_UNORM);
        Image second = gpu.resources.image({4, 4}, VK_FORMAT_R8_UNORM, Fill::draw);
        const VkImage was = second.handle();
        second = std::move(first);
        rows.row(std::format("moved onto one that holds an image: the old one holds what "
                             "it held: {}",
                             show(first.handle() == was, names)));
      }
      {
        Image first = gpu.resources.image({8, 8}, VK_FORMAT_R8G8B8A8_UNORM, Fill::store);
        Image &same = first;
        first = std::move(same);
        rows.row("moved onto itself -> " + show(first, names));
      }
    });
  });

  // A mip chain's shape: the levels an image holds down to a pixel, each level's size,
  // and where each starts in the one buffer an upload stages them in.
  bench.sweep("Resources.cpp/mip_level_count", mip_level_count);
  bench.check("Resources.cpp/mip_level_extent", [](Rows &rows) {
    for (const VkExtent2D extent :
         {VkExtent2D{1, 1}, VkExtent2D{5, 3}, VkExtent2D{64, 64}, VkExtent2D{4096, 1}}) {
      std::string levels;
      for (std::uint32_t level = 0; level < mip_level_count(extent); ++level) {
        const VkExtent2D size = mip_level_extent(extent, level);
        levels += std::format(
            " {}x{} at {}", size.width, size.height, mip_level_offset(extent, 4, level));
      }
      rows.row(std::format("{}x{} ->{}; all {} bytes",
                           extent.width,
                           extent.height,
                           levels,
                           mip_level_offset(extent, 4, mip_level_count(extent))));
    }
  });
  bench.sweep("Resources.cpp/makes_mip_levels", makes_mip_levels, domain::ImageFormat{});

  // sRGB's decode, worked out by the compiler, and the encode back: each 8-bit value the
  // nearest to its own light, and lights between two values to the nearer, the darker
  // on a tie.
  bench.check("Resources.cpp/srgb_encoded", [](Rows &rows) {
    rows.row(std::format("fifth_root of 32 -> {}; of 1 -> {}; of 0.0064 -> {}",
                         fifth_root(32.0),
                         fifth_root(1.0),
                         fifth_root(0.0064)));
    rows.row(std::format("decoded: 0 -> {}, 1 -> {}, 10 -> {}, 11 -> {}, 128 -> {}, "
                         "254 -> {}, 255 -> {}",
                         srgb_decoded[0],
                         srgb_decoded[1],
                         srgb_decoded[10],
                         srgb_decoded[11],
                         srgb_decoded[128],
                         srgb_decoded[254],
                         srgb_decoded[255]));
    for (const std::uint64_t light :
         {0ULL, 9ULL, 10ULL, 11ULL, 30ULL, 32768ULL, 65535ULL, 65536ULL, 1ULL << 40})
      rows.row(std::format("encoded {} -> {}", light, srgb_encoded(light)));
    int back = 0;
    int rising = 0;
    for (std::uint64_t value = 0; value <= 255; ++value) {
      back += srgb_encoded(srgb_decoded[value]) == value ? 1 : 0;
      rising += value == 0 || srgb_decoded[value] > srgb_decoded[value - 1] ? 1 : 0;
    }
    rows.row(std::format(
        "of 256 values: {} encode back to themselves, {} decode above the one before",
        back,
        rising));
  });

  // The levels past the first, made on the CPU in integers: a 2 by 2 box averaged, an
  // odd side's last row or column left out, a color weighed by its alpha, and sRGB
  // averaged in linear light. Each level's bytes, in hex.
  bench.check("Resources.cpp/make_mip_levels", [](Rows &rows) {
    const auto made_from = [&](std::string_view what,
                               VkExtent2D extent,
                               VkFormat format,
                               std::vector<std::uint8_t> first) {
      const std::uint32_t channels = mip_channels(format);
      const std::uint32_t levels = mip_level_count(extent);
      const std::uint32_t bytes = channels == 0 ? 4 : channels;
      std::vector<std::byte> chain(mip_level_offset(extent, bytes, levels),
                                   std::byte{0xEE});
      for (std::size_t at = 0; at < first.size(); ++at)
        chain[at] = std::byte{first[at]};
      make_mip_levels(chain, extent, format, levels);
      std::string text;
      for (std::uint32_t level = 1; level < levels; ++level) {
        const VkExtent2D size = mip_level_extent(extent, level);
        text += std::format(" | level {}:", level);
        const VkDeviceSize from = mip_level_offset(extent, bytes, level);
        for (VkDeviceSize at = from;
             at < from + VkDeviceSize{size.width} * size.height * bytes;
             ++at)
          text += std::format(" {:02x}", std::to_integer<unsigned>(chain[at]));
      }
      rows.row(std::format("{} ->{}", what, text));
    };
    made_from("R8 3x3, 0..8",
              {3, 3},
              VK_FORMAT_R8_UNORM,
              {0, 10, 200, 20, 30, 200, 200, 200, 200});
    made_from(
        "R8G8 5x1", {5, 1}, VK_FORMAT_R8G8_UNORM, {0, 255, 1, 254, 2, 253, 3, 252, 9, 9});
    made_from("RGBA8 2x2, one texel opaque red, three transparent white",
              {2, 2},
              VK_FORMAT_R8G8B8A8_UNORM,
              {255, 0, 0, 255, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0});
    made_from("RGBA8 2x2, all transparent, red and blue",
              {2, 2},
              VK_FORMAT_R8G8B8A8_UNORM,
              {255, 0, 0, 0, 255, 0, 0, 0, 0, 0, 255, 0, 0, 0, 255, 0});
    made_from("RGBA8 sRGB 2x2, black and white, opaque",
              {2, 2},
              VK_FORMAT_R8G8B8A8_SRGB,
              {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255});
    made_from("RGBA8 UNORM 2x2, black and white, opaque",
              {2, 2},
              VK_FORMAT_R8G8B8A8_UNORM,
              {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255});
    made_from("B8G8R8A8, which the CPU makes no levels of: left as it was",
              {2, 2},
              VK_FORMAT_B8G8R8A8_UNORM,
              {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
  });

  // The depth format the device takes, an image of it, and images with mip levels: as
  // many as the size holds, and one more, which callers never ask (levels_of refuses
  // it), as the layer answers it. Not none: lavapipe ends the process on an image of no
  // levels, after the layer's error, so the rows would end wherever a context reached.
  bench.check("Resources.cpp/Resources::image, depth and mip levels", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      Names &names = rows.names();
      rows.row("depth format -> " + show(gpu.resources.depth_format(), names));
      rows.row("a depth image -> " +
               made(
                   [&] {
                     return gpu.resources.image(
                         {64, 32}, gpu.resources.depth_format(), Fill::depth);
                   },
                   names)
                   .text);
      for (const std::uint32_t levels : {1U, 7U, 8U})
        rows.row(
            std::format("64x64 RGBA8 with {} levels -> {}",
                        levels,
                        made(
                            [&] {
                              Image image = gpu.resources.image(
                                  {64, 64}, VK_FORMAT_R8G8B8A8_UNORM, Fill::copy, levels);
                              return std::format("holds {} levels", image.mip_levels());
                            },
                            names)
                            .text));
    });
  });

  carries_layer(bench, "Resources.cpp/Resources::image");
  carries_layer(bench, "Resources.cpp/Resources::samples");
  carries_layer(bench, "Resources.cpp/Resources::image, depth and mip levels");
}

} // namespace VP::bench
