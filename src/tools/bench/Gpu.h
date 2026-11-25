#pragma once

#include "baseclasses/Log.h"
#include "baseclasses/Mechanics.h"
#include "baseclasses/Platform.h"
#include "baseclasses/Resources.h"
#include "tools/bench/Bench.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The bench's GPU: a device made as a headless run makes it (V07), once for a
// check's rows, and only on lavapipe, Mesa's Vulkan on the CPU, which bench.py names in
// VK_DRIVER_FILES: never this machine's own GPU, whose memory a row asking for too much
// would take from a person's session. A debug build runs with validation on, as vulpen's
// does where the layer is installed, so what the layer says of a call is printed in that
// call's row (RVK00), and what it says of objects left once the device goes, in the
// check's last row.
//
// Where no device can be made so, a check writes one row that starts with ??, saying
// why, and bench.py counts it as not run here, never as a difference: Windows CI has no
// Vulkan driver at all.

namespace VP::bench {

// The GPU's own inputs, which every case of a GPU file shares: where a buffer lives, what
// fills an image, its size and its format.
template <> struct Gen<Memory> {
  using type = Memory;
  static std::vector<Memory> edges() {
    return {Memory::device, Memory::upload, Memory::readback};
  }
  static Memory random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<Fill> {
  using type = Fill;
  static std::vector<Fill> edges() {
    return {Fill::copy, Fill::draw, Fill::store, Fill::depth};
  }
  static Fill random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<VkExtent2D> {
  using type = VkExtent2D;
  static std::vector<VkExtent2D> edges() {
    constexpr std::uint32_t most = std::numeric_limits<std::uint32_t>::max();
    return {{64, 64},
            {1, 1},
            {0, 0},
            {0, 64},
            {64, 0},
            {4096, 1},
            {16384, 1},
            {1, 16384},
            {16385, 1},
            {1, 16385},
            {most, 1},
            {most, most}};
  }
  static VkExtent2D random(Random &random) {
    return {static_cast<std::uint32_t>(1 + random.below(256)),
            static_cast<std::uint32_t>(1 + random.below(256))};
  }
};

// The formats a node's image takes, as runtime/Bound.h's pixel_formats lists them, which
// a case of baseclasses does not include, as includes flow up (A06); then the window's,
// none, a depth and a compressed one, which no caller passes. Each by its name.
inline constexpr std::array<std::pair<VkFormat, std::string_view>, 14> formats{{
    {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
    {VK_FORMAT_R8_UNORM, "R8_UNORM"},
    {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM"},
    {VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB"},
    {VK_FORMAT_R16_SFLOAT, "R16_SFLOAT"},
    {VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT"},
    {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
    {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT"},
    {VK_FORMAT_R32G32_SFLOAT, "R32G32_SFLOAT"},
    {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT"},
    {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB"},
    {VK_FORMAT_UNDEFINED, "UNDEFINED"},
    {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
    {VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1_RGB_UNORM_BLOCK"},
}};

template <> struct Gen<VkFormat> {
  using type = VkFormat;
  static std::vector<VkFormat> edges() {
    std::vector<VkFormat> all;
    for (const auto &[format, name] : formats)
      all.push_back(format);
    return all;
  }
  static VkFormat random(Random &random) {
    return formats[random.below(formats.size())].first;
  }
};

template <> struct Shown<Memory> {
  static std::string text(Memory memory, Names &) {
    constexpr std::array<std::string_view, 3> words{"device", "upload", "readback"};
    const auto index = static_cast<std::size_t>(memory);
    return index < words.size() ? std::string(words[index])
                                : "memory " + number_text(index);
  }
};

template <> struct Shown<Fill> {
  static std::string text(Fill fill, Names &) {
    constexpr std::array<std::string_view, 4> words{"copy", "draw", "store", "depth"};
    const auto index = static_cast<std::size_t>(fill);
    return index < words.size() ? std::string(words[index])
                                : "fill " + number_text(index);
  }
};

template <> struct Shown<VkExtent2D> {
  static std::string text(VkExtent2D extent, Names &) {
    return std::format("{}x{}", extent.width, extent.height);
  }
};

template <> struct Shown<VkFormat> {
  static std::string text(VkFormat format, Names &) {
    const auto named = std::ranges::find_if(
        formats, [&](const auto &each) { return each.first == format; });
    return named != formats.end() ? std::string(named->second)
                                  : "format " + number_text(static_cast<int>(format));
  }
};

class Gpu {
public:
  explicit Gpu(const Log &made_with)
      : log(made_with), mechanics(made_with, nullptr, "cpu"),
        resources(mechanics.instance(),
                  mechanics.physical_device(),
                  mechanics.device(),
                  mechanics.budget()) {}

  const Log &log;
  Mechanics mechanics;
  Resources resources;
};

// Whether bench.py named lavapipe, the only driver a bench makes a device on; else one
// row saying it did not, for a check that makes its device itself, as an Engine.
inline bool on_lavapipe(Rows &rows) {
  const char *const driver = std::getenv("VK_DRIVER_FILES");
  if (driver != nullptr && *driver != '\0')
    return true;
  rows.row("?? needs lavapipe: bench.py names its driver in VK_DRIVER_FILES where the "
           "build found one");
  return false;
}

// A device on lavapipe, or none, with a row saying why; what making it printed is
// dropped, as Mechanics' own case shows it.
inline std::unique_ptr<Gpu> lavapipe(Rows &rows, const Log &log) {
  if (!on_lavapipe(rows))
    return nullptr;
  std::unique_ptr<Gpu> gpu;
  try {
    gpu = rows.quiet([&] { return std::make_unique<Gpu>(log); });
  } catch (const std::exception &failure) {
    rows.row(std::format("?? needs lavapipe: no device: {}", escaped(failure.what())));
    return nullptr;
  }
  if (gpu->mechanics.kind() != "cpu") {
    rows.row(std::format("?? needs lavapipe: the driver named gives {}",
                         escaped(gpu->mechanics.describe())));
    return nullptr;
  }
  return gpu;
}

// Body's rows on a device made for them, then a row as the device goes, which shows what
// the layer and the allocator said of anything left (A01, A03).
template <class Body> void on_gpu(Rows &rows, Body &&body) {
  auto log = rows.quiet([] { return std::make_unique<Log>(Level::error, "bench"); });
  if (std::unique_ptr<Gpu> gpu = lavapipe(rows, *log)) {
    body(*gpu);
    gpu.reset();
    rows.row("the device goes -> done");
  }
  rows.quiet([&] {
    log.reset();
    return 0;
  });
}

// Body's rows with a window on the display bench.py's Xvfb gives, which no person sees,
// on the check's device, which presents to it on lavapipe; or none, with one row saying
// why, as where Xvfb is not installed.
template <class Body> void on_window(Rows &rows, Body &&body) {
  const char *const screen = std::getenv("DISPLAY");
  if (screen == nullptr || *screen == '\0') {
    rows.row("?? needs a display: bench.py starts Xvfb where it is installed");
    return;
  }
  on_gpu(rows, [&](Gpu &gpu) {
    constexpr VkExtent2D size{640, 480};
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<Window> window;
    try {
      desktop = rows.quiet([] { return std::make_unique<Desktop>(); });
      window =
          rows.quiet([&] { return std::make_unique<Window>(*desktop, "bench", size); });
    } catch (...) {
      rows.row("?? needs a display: " + thrown());
      return;
    }
    body(gpu, *window);
    rows.quiet([&] {
      window.reset();
      desktop.reset();
      return 0;
    });
  });
}

// An id whose rows open a window, which GLFW makes on the process's main thread only:
// the contexts that run the rows on other threads left out for it.
inline void takes_the_main_thread(Bench &bench, const std::string &id) {
  for (const char *const context : {"thread", "threads"})
    bench.leaves_out(id, context, "GLFW opens a window on the main thread only");
}

// An id whose rows carry what the validation layer said, which the locale context and a
// release build change: both left out for it.
inline void carries_layer(Bench &bench, const std::string &id) {
  bench.leaves_out(id,
                   "locale",
                   "the validation layer prints its numbers in the process's "
                   "locale, which vulpen never sets");
  bench.leaves_out(id,
                   "release",
                   "a release build runs without the validation layer, "
                   "whose words these rows carry");
}

} // namespace VP::bench
