#include "baseclasses/Mechanics.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// baseclasses/Mechanics.cpp: the instance, the device a run chooses, and the one frame in
// flight, headless (V07), on lavapipe (bench/Gpu.h), which is this process's only GPU;
// a window's surface and the device chosen for it are Swapchain's case, under Xvfb.

namespace VP::bench {

namespace domain {
// A kind of GPU a person prefers, as .vulpen/gpu.ini holds it.
struct Prefer {};
// An extension or a layer, by its name.
struct Extension {};
} // namespace domain

template <> struct Gen<domain::Prefer> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {
        "", "cpu", "discrete", "integrated", "virtual", "other", "gpu", "CPU", " cpu"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::Extension> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
            "",
            "VK_KHR_SWAPCHAIN",
            "VK_KHR_swapchain ",
            "VK_LAYER_KHRONOS_validation"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<VkResult> {
  using type = VkResult;
  static std::vector<VkResult> edges() {
    return {VK_SUCCESS,
            VK_ERROR_OUT_OF_DEVICE_MEMORY,
            VK_ERROR_DEVICE_LOST,
            VK_NOT_READY,
            VK_TIMEOUT,
            VK_INCOMPLETE,
            VK_SUBOPTIMAL_KHR,
            VK_ERROR_OUT_OF_DATE_KHR};
  }
  static VkResult random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

namespace {

// The GPUs the instance sees.
std::vector<VkPhysicalDevice> devices_of(VkInstance instance) {
  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  return devices;
}

std::string families_text(VkPhysicalDevice device) {
  std::string text;
  for (const VkQueueFamilyProperties &family : families_of(device))
    text += std::format("{}[flags {:#x}, {} queues, {} timestamp bits]",
                        text.empty() ? "" : " ",
                        family.queueFlags,
                        family.queueCount,
                        family.timestampValidBits);
  return text;
}

// What a call said: done, or what it threw.
template <class F> std::string said(F &&call) {
  try {
    call();
    return "done";
  } catch (...) {
    return thrown();
  }
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Mechanics.cpp/check", [](VkResult result, const std::string &call) {
    check(result, call.c_str());
  });

  // The device the instance sees, as each helper reads it; the instance's, not the one a
  // Mechanics made, so each helper runs on its own.
  bench.check("Mechanics.cpp/the GPU's facts", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const VkInstance instance = gpu.mechanics.instance();
      const std::vector<VkPhysicalDevice> devices = devices_of(instance);
      rows.row(std::format("GPUs seen -> {}", devices.size()));
      for (const VkPhysicalDevice device : devices) {
        rows.row("describe -> " + escaped(describe(device)));
        rows.row(std::format("kind_of -> {}", kind_of(device)));
        rows.row(
            std::format("meets_floor -> {}", show(meets_floor(device), rows.names())));
        rows.row(std::format("has_swapchain -> {}",
                             show(has_swapchain(device), rows.names())));
        rows.row("families_of -> " + families_text(device));
        const std::optional<std::uint32_t> family = queue_family(device, VK_NULL_HANDLE);
        rows.row("queue_family, headless -> " + show(family, rows.names()));
        const char *const why = unfit(device, VK_NULL_HANDLE);
        rows.row(std::format("unfit, headless -> {}", why ? why : "fit"));
      }
      rows.row(std::format("Mechanics: describe {}, kind {}, budget {}, timestamp "
                           "period {} ns, {} timestamp bits",
                           escaped(gpu.mechanics.describe()),
                           gpu.mechanics.kind(),
                           show(gpu.mechanics.budget(), rows.names()),
                           number_text(gpu.mechanics.timestamp_period()),
                           gpu.mechanics.timestamp_bits()));
    });
  });

  bench.check("Mechanics.cpp/has_extension", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(
          rows,
          "Mechanics.cpp/has_extension",
          gpu,
          [](Gpu &on, const std::string &name) {
            return has_extension(on.mechanics.physical_device(), name.c_str());
          },
          domain::Extension{});
    });
  });

  bench.sweep(
      "Mechanics.cpp/layer_installed",
      [](const std::string &name) { return layer_installed(name.c_str()); },
      domain::Extension{});

  bench.check("Mechanics.cpp/rank and best_of", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(
          rows,
          "Mechanics.cpp/rank",
          gpu,
          [](Gpu &on, const std::string &prefer) {
            const VkPhysicalDevice device = on.mechanics.physical_device();
            const std::vector<VkPhysicalDevice> twice{device, device};
            return std::format("rank {}, best of it twice is it: {}",
                               rank(device, prefer),
                               best_of(twice, prefer) == device);
          },
          domain::Prefer{});
    });
  });

  // The GPUs that meet the floor, as the log at debug names each.
  bench.check("Mechanics.cpp/fit_devices", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      auto debug = rows.quiet([] { return std::make_unique<Log>(Level::debug, "fit"); });
      rows.row("fit_devices, headless -> " + said([&] {
                 fit_devices(gpu.mechanics.instance(), VK_NULL_HANDLE, *debug);
               }));
      rows.quiet([&] {
        debug.reset();
        return 0;
      });
    });
  });

  // A device made and destroyed again, on the device the instance chose.
  bench.check("Mechanics.cpp/create_device", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const VkPhysicalDevice physical = gpu.mechanics.physical_device();
      rows.row("create_device on family 0, then destroyed -> " + said([&] {
                 const VkDevice device = create_device(physical, 0);
                 vkDestroyDevice(device, nullptr);
               }));
    });
  });

  // A whole Mechanics for each preference, beside the check's own: what it chose, and
  // what each next launch would take.
  bench.check("Mechanics.cpp/Mechanics::Mechanics", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      sweep_in(
          rows,
          "Mechanics.cpp/Mechanics::Mechanics",
          gpu,
          [](Gpu &on, const std::string &prefer) {
            try {
              const Mechanics mechanics(on.log, nullptr, prefer);
              return std::format("{}; takes {} for none, {} for discrete, {} for cpu",
                                 mechanics.kind(),
                                 mechanics.takes(""),
                                 mechanics.takes("discrete"),
                                 mechanics.takes("cpu"));
            } catch (...) {
              return thrown();
            }
          },
          domain::Prefer{});
    });
  });

  // One frame in flight: recorded, submitted, waited for; again; and what record and
  // submit do out of that order, which the engine's callers keep (VK02).
  bench.check("Mechanics.cpp/a frame", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Mechanics &mechanics = gpu.mechanics;
      for (int frame = 0; frame < 3; ++frame)
        rows.row(std::format("frame {}: record, submit, wait -> {}", frame, said([&] {
                               mechanics.record();
                               mechanics.submit();
                               mechanics.wait();
                             })));
      rows.row("wait with nothing in flight -> " + said([&] { mechanics.wait(); }));
      rows.row("wait_idle -> " + said([&] { mechanics.wait_idle(); }));
      rows.row("record while the last frame is in flight -> " + said([&] {
                 mechanics.record();
                 mechanics.submit();
                 mechanics.record();
               }));
      rows.row("submit what is recorded, wait -> " + said([&] {
                 mechanics.submit();
                 mechanics.wait();
               }));
      rows.row("submit again with nothing recorded -> " + said([&] {
                 mechanics.submit();
                 mechanics.wait();
               }));
    });
  });
  carries_layer(bench, "Mechanics.cpp/a frame");
}

} // namespace VP::bench
