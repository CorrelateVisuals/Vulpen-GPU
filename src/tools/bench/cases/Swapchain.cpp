#include "baseclasses/Swapchain.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"

#include <format>
#include <memory>
#include <optional>
#include <string>

// baseclasses/Swapchain.cpp: the window output, on a window of Xvfb's (bench/Gpu.h's
// on_window), which no person sees, presented by lavapipe; with synchronization
// validation on, so each frame's acquire, render pass, submit and present are checked.
// What the window's size and the display do between frames (a resize, a minimize) the
// bench cannot make Xvfb do; those paths are read.

namespace VP::bench {

namespace {

template <class F> std::string said(F &&call) {
  try {
    call();
    return "done";
  } catch (...) {
    return thrown();
  }
}

// Frames as the engine runs them with a window: acquire, a render pass that clears the
// image, submit waiting for the acquire and signalling the image's own semaphore,
// present, and the fence before the next. Its log is at debug, so a frame that makes
// the swapchain again prints so in its row: at one size, none does.
void frames(Rows &rows, VkSampleCountFlagBits samples, bool vsync) {
  on_window(rows, [&](Gpu &gpu, const Window &window) {
    auto debug = rows.quiet([] { return std::make_unique<Log>(Level::debug, "frames"); });
    auto swapchain = rows.quiet([&] {
      return std::make_unique<Swapchain>(
          *debug, gpu.mechanics, gpu.resources, window, samples, vsync);
    });
    Swapchain &frames = *swapchain;
    for (int frame = 0; frame < 5; ++frame) {
      std::optional<Target> target;
      const std::string ran = said([&] {
        target = frames.acquire();
        const VkCommandBuffer commands = gpu.mechanics.record();
        if (target) {
          frames.begin(commands, *target);
          vkCmdEndRenderPass(commands);
        }
        if (target) {
          gpu.mechanics.submit(target->acquired, target->rendered);
          frames.present(*target);
        } else {
          gpu.mechanics.submit();
        }
        gpu.mechanics.wait();
      });
      rows.row(std::format("frame {} -> {}; {}",
                           frame,
                           ran,
                           target ? std::format("an image of {}x{}",
                                                target->extent.width,
                                                target->extent.height)
                                  : std::string("no image")));
    }
    rows.quiet([&] {
      swapchain.reset();
      debug.reset();
      return 0;
    });
  });
}

} // namespace

void cases(Bench &bench) {
  // What the window's surface offers, and what the swapchain takes of it; made, gone,
  // and made again, as Engine::close and Engine::open do.
  bench.check("Swapchain.cpp/Swapchain::Swapchain", [](Rows &rows) {
    on_window(rows, [&](Gpu &gpu, const Window &window) {
      const VkSurfaceKHR surface = window.surface(gpu.mechanics.instance());
      const VkSurfaceFormatKHR format =
          pick_format(gpu.mechanics.physical_device(), surface);
      const VkPhysicalDevice device = gpu.mechanics.physical_device();
      rows.row(std::format("the surface: format {}, color space {}, present mode {}, the "
                           "device presents to it: {}",
                           show(format.format, rows.names()),
                           static_cast<int>(format.colorSpace),
                           static_cast<int>(pick_present_mode(device, surface, false)),
                           gpu.mechanics.presents(surface)));
      // With vsync, FIFO whatever the surface offers: a frame each refresh.
      rows.row(std::format("present mode with vsync {}",
                           static_cast<int>(pick_present_mode(device, surface, true))));
      vkDestroySurfaceKHR(gpu.mechanics.instance(), surface, nullptr);
      auto log = rows.quiet([] { return std::make_unique<Log>(Level::info, "window"); });
      // Twice at one sample and vsync off, then with four samples, which lavapipe's
      // color attachments take, and vsync.
      for (int time = 0; time < 3; ++time) {
        const bool multisampled = time == 2;
        std::unique_ptr<Swapchain> swapchain;
        rows.row(std::format("made{}, time {} -> {}",
                             multisampled ? " with 4 samples and vsync" : "",
                             time,
                             said([&] {
                               swapchain = std::make_unique<Swapchain>(
                                   *log,
                                   gpu.mechanics,
                                   gpu.resources,
                                   window,
                                   multisampled ? VK_SAMPLE_COUNT_4_BIT
                                                : VK_SAMPLE_COUNT_1_BIT,
                                   multisampled);
                             })));
        if (!swapchain)
          continue;
        const std::string render_pass = show(swapchain->render_pass(), rows.names());
        const std::string gone = said([&] { swapchain.reset(); });
        rows.row(std::format("its render pass {}; then gone -> {}", render_pass, gone));
      }
      rows.quiet([&] {
        log.reset();
        return 0;
      });
    });
  });

  bench.check("Swapchain.cpp/frames",
              [](Rows &rows) { frames(rows, VK_SAMPLE_COUNT_1_BIT, false); });
  // Four samples a pixel, resolved into the window's image as the pass ends, and FIFO.
  bench.check("Swapchain.cpp/frames, multisampled",
              [](Rows &rows) { frames(rows, VK_SAMPLE_COUNT_4_BIT, true); });
  for (const char *const id :
       {"Swapchain.cpp/frames", "Swapchain.cpp/frames, multisampled"})
    carries_layer(bench, id);
  for (const char *const id : {"Swapchain.cpp/Swapchain::Swapchain",
                               "Swapchain.cpp/frames",
                               "Swapchain.cpp/frames, multisampled"})
    takes_the_main_thread(bench, id);
}

} // namespace VP::bench
