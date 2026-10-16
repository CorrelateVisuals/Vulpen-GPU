#include "baseclasses/Engine.h"
#include "baseclasses/Log.h"
#include "baseclasses/Passes.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Platform.h"
#include "baseclasses/Shader.h"
#include "commit.h"

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string_view>

// A triangle drawn with the baseclasses alone, without the runtime, its views or its
// commands, so the GPU layer is shown whole by itself (RA00).
namespace {

constexpr VkExtent2D window_size{.width = 1280, .height = 720};
constexpr std::uint32_t corners = 3;

// The triangle takes none of the window's events; its close button still ends the run.
class Unread final : public VP::WindowInput {
  void key(std::string_view, bool) override {}
  void text(std::string_view) override {}
  void pointer(float, float) override {}
  void button(std::string_view, bool) override {}
  void wheel(float, float) override {}
  void focus(bool) override {}
};

void draw(const VP::Log &log) {
  const VP::Window window("triangle - baseclasses", window_size);
  VP::Engine engine(log, &window);
  // The build puts the shaders beside the binary, so they are found from it (RP02).
  const std::filesystem::path folder = VP::Files::executable().parent_path();
  const VP::Pipeline pipeline(engine.pipelines(),
                              VP::Shader(folder / "Triangle.vert.spv"),
                              VP::Shader(folder / "Triangle.frag.spv"),
                              engine.render_pass());
  const std::array passes{VP::Pass{.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   .pipeline = pipeline.handle(),
                                   .vertex_count = corners}};
  Unread input;
  // One frame in flight: each waits for the one before, whose commands it records over.
  for (std::uint64_t frame = 0; window.poll(input); ++frame) {
    engine.wait();
    engine.run({}, {}, passes, frame, {}, {}, {});
  }
  // The pipeline goes before the engine, so the last frame must be done with it.
  engine.wait();
}

} // namespace

int main() {
  const VP::Log log(VP::Level::warn, "triangle " VULPEN_COMMIT);
  try {
    draw(log);
    return 0;
  } catch (const std::exception &error) {
    log.write(VP::Level::error, VP::Tag::run, error.what());
    return 1;
  }
}
