#include "baseclasses/Engine.cpp"

#include "baseclasses/Shader.h"
#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// baseclasses/Engine.cpp: one frame of passes, headless (V07), on lavapipe, with
// synchronization validation on (bench.py's layer settings), so a barrier missing
// between a frame's clears, copies, dispatches and render passes shows in the row of the
// frame that recorded it. The dispatch is wave's, bound as the schedule binds it: its two
// floats and its buffer's address in its pass block. The window's frames are Swapchain's
// case, under Xvfb.

namespace VP::bench {

namespace {

// An Engine of its own on lavapipe, which the check's device only vouches for.
struct Frames {
  explicit Frames(const Log &log) : engine(log, nullptr, "cpu") {}
  Engine engine;

  // A frame as Runtime runs one: wait for the last, then run this one.
  std::string run(std::span<const VkBuffer> clears,
                  std::span<const Copy> copies,
                  std::span<const Pass> passes,
                  std::uint64_t frame = 0) {
    try {
      engine.wait();
      engine.run(clears,
                 copies,
                 passes,
                 frame,
                 static_cast<double>(frame) / 60,
                 {0, 0},
                 {64, 64});
      engine.wait();
      return "done";
    } catch (...) {
      return thrown();
    }
  }
};

// What the frame left in a host-visible buffer: its first floats.
std::string floats(const Buffer &buffer, std::size_t count) {
  buffer.invalidate();
  std::string text;
  for (std::size_t index = 0; index < count; ++index) {
    float value = 0;
    std::memcpy(&value, buffer.bytes().data() + index * sizeof value, sizeof value);
    text += (index ? "," : "") + number_text(value);
  }
  return "[" + text + "]";
}

// Wave's shader, bound as the schedule binds it: amplitude, phase, and the address of
// the buffer it writes, at the offsets reflection gives.
struct Wave {
  Wave(const Engine &engine, const std::filesystem::path &file)
      : shader(file), pipeline(engine.pipelines(), shader),
        values(engine.resources().buffer(std::uint64_t{invocations} * sizeof(float),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         Memory::readback)),
        block(engine.pipelines(), shader.block_size()) {
    for (const Field &field : shader.fields()) {
      std::byte *const at = block.bytes().data() + field.offset;
      if (field.name == "amplitude") {
        const float amplitude = 1;
        std::memcpy(at, &amplitude, sizeof amplitude);
      } else if (field.buffer()) {
        const VkDeviceAddress address = values.address();
        std::memcpy(at, &address, sizeof address);
      }
    }
    block.flush();
    pass = {.pipeline = pipeline.handle(),
            .block = block.set(),
            .groups = invocations / group,
            .reads = {values.handle()},
            .writes = {values.handle()}};
  }

  static constexpr std::uint32_t invocations = 1024;
  static constexpr std::uint32_t group = 64; // Wave.comp's local_size_x
  Shader shader;
  Pipeline pipeline;
  Buffer values;
  PassBlock block;
  Pass pass;
};

// What draws left in an image, read as a hosted view's picture is taken: the snapshot
// part's shader samples it, at the frame's size, into a buffer the CPU reads once the
// frame's fence passed.
struct Picture {
  Picture(const Engine &engine, const std::filesystem::path &file, const Drawn &drawn)
      : shader(file), pipeline(engine.pipelines(), shader),
        pixels(
            engine.resources().buffer(std::uint64_t{side} * side * sizeof(std::uint32_t),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      Memory::readback)),
        block(engine.pipelines(), shader.block_size()) {
    const std::uint32_t slot = drawn.sampled().slot();
    const std::uint32_t shoot = 1;
    const VkDeviceAddress address = pixels.address();
    for (const Field &field : shader.fields()) {
      std::byte *const at = block.bytes().data() + field.offset;
      if (field.name == "picture")
        std::memcpy(at, &slot, sizeof slot);
      else if (field.name == "shoot")
        std::memcpy(at, &shoot, sizeof shoot);
      else if (field.buffer())
        std::memcpy(at, &address, sizeof address);
    }
    block.flush();
    pass = {.pipeline = pipeline.handle(),
            .block = block.set(),
            .groups = side * side / group,
            .writes = {pixels.handle()}};
  }

  // The pixels at the points given, x then y from the top left, as the sRGB bytes
  // packUnorm4x8 leaves in memory: red first.
  std::string at(std::initializer_list<std::array<std::uint32_t, 2>> points) const {
    pixels.invalidate();
    std::string text;
    for (const auto [x, y] : points) {
      std::array<std::uint8_t, sizeof(std::uint32_t)> rgba{};
      std::memcpy(
          rgba.data(), pixels.bytes().data() + (y * side + x) * rgba.size(), rgba.size());
      text += std::format("{}({},{}) #", text.empty() ? "" : ", ", x, y);
      for (const std::uint8_t channel : rgba)
        text += std::format("{:02x}", channel);
    }
    return text;
  }

  static constexpr std::uint32_t side = 64;  // the size Frames::run gives a frame
  static constexpr std::uint32_t group = 64; // Snapshot.comp's local_size_x
  Shader shader;
  Pipeline pipeline;
  Buffer pixels;
  PassBlock block;
  Pass pass;
};

} // namespace

void cases(Bench &bench) {
  bench.check("Engine.cpp/Engine::Engine", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Frames frames(gpu.log);
      const Engine &engine = frames.engine;
      rows.row(std::format("headless: device {}, kind {}, takes {} for discrete, render "
                           "pass {}, {} times",
                           escaped(engine.device()),
                           engine.kind(),
                           engine.takes("discrete"),
                           show(engine.render_pass(), rows.names()),
                           engine.times().size()));
    });
  });

  // Frames of nothing, then of clears: a buffer the CPU filled reads zeroes once a frame
  // cleared it, and a buffer cleared twice in one frame, which no schedule lists, shows
  // what synchronization validation says of a write after a write.
  bench.check("Engine.cpp/Engine::run clears", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      Frames frames(gpu.log);
      for (std::uint64_t frame = 0; frame < 3; ++frame)
        rows.row(std::format(
            "frame {} of nothing -> {}", frame, frames.run({}, {}, {}, frame)));
      const Buffer buffer = frames.engine.resources().buffer(
          64,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          Memory::readback);
      std::ranges::fill(buffer.bytes(), std::byte{0xab});
      buffer.flush();
      const VkBuffer cleared = buffer.handle();
      const std::string ran = frames.run(std::span(&cleared, 1), {}, {});
      rows.row(
          std::format("a filled buffer cleared -> {}; reads {}", ran, floats(buffer, 4)));
      const std::array twice{cleared, cleared};
      rows.row("the same buffer cleared twice in a frame -> " +
               frames.run(twice, {}, {}));
    });
  });
  carries_layer(bench, "Engine.cpp/Engine::run clears");

  // Pixels the CPU put in a buffer, copied into an image before the passes; zeroes into
  // an image a dispatch stores into; both again the next frame.
  bench.check("Engine.cpp/Engine::run copies", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      Frames frames(gpu.log);
      const Resources &resources = frames.engine.resources();
      const Image copied = resources.image({4, 4}, VK_FORMAT_R8G8B8A8_UNORM, Fill::copy);
      const Image stored = resources.image({4, 4}, VK_FORMAT_R8G8B8A8_UNORM, Fill::store);
      const Buffer pixels =
          resources.buffer(4 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::upload);
      std::ranges::fill(pixels.bytes(), std::byte{0x7f});
      pixels.flush();
      const std::array copies{
          Copy{.from = pixels.handle(), .to = copied.handle(), .extent = {4, 4}},
          Copy{.to = stored.handle(), .extent = {4, 4}}};
      for (std::uint64_t frame = 0; frame < 2; ++frame)
        rows.row(std::format("frame {}: a copy and a zeroing -> {}",
                             frame,
                             frames.run({}, copies, {}, frame)));
    });
  });
  carries_layer(bench, "Engine.cpp/Engine::run copies");

  // Draws into an image of their own, each run of them in one render pass: one that
  // only clears it, as the schedule's clears() makes for a hosted window, no pipeline and
  // no instance, and one with no pipeline but an instance, which only clears it too, as a
  // dispatch with none records nothing; the triangle, its corners read from a buffer
  // through its pass block; two of them; and draws into an image with no size yet, which
  // record nothing.
  bench.check("Engine.cpp/Engine::run draws into an image", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const std::filesystem::path folder =
          rows.repo() / "out" / "build" / "debug" / "views" / "triangle" / "triangle";
      if (!std::filesystem::exists(folder / "Triangle.vert.spv")) {
        rows.row("no Triangle.vert.spv: the debug build compiles it");
        return;
      }
      Frames frames(gpu.log);
      const Pipelines &pipelines = frames.engine.pipelines();
      const Resources &resources = frames.engine.resources();
      const Drawn drawn(pipelines,
                        resources.image({64, 64}, VK_FORMAT_R8G8B8A8_UNORM, Fill::draw));
      const Offscreen sized{.render_pass =
                                pipelines.render_pass(VK_FORMAT_R8G8B8A8_UNORM),
                            .framebuffer = drawn.framebuffer(),
                            .extent = {64, 64}};
      const Offscreen unsized{.render_pass = sized.render_pass};
      const Shader vertex(folder / "Triangle.vert.spv");
      const Shader fragment(folder / "Triangle.frag.spv");
      const std::array stages{
          ShaderStage{.stage = VK_SHADER_STAGE_VERTEX_BIT, .shader = &vertex},
          ShaderStage{.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .shader = &fragment}};
      const Pipeline pipeline(pipelines, stages, sized.render_pass);
      // A triangle's corners, a position in clip space and a color each, as Triangle.vert
      // reads them, x across from the middle, in a buffer its pass block holds.
      struct Corners {
        Buffer buffer;
        PassBlock block;
      };
      const auto corners_at = [&](float x) {
        const std::array<float, 24> corners{x - 0.5f, -0.5f, 0, 1, 1, 0, 0, 1,
                                            x + 0.5f, -0.5f, 0, 1, 0, 1, 0, 1,
                                            x,        0.5f,  0, 1, 0, 0, 1, 1};
        Corners made{.buffer = resources.buffer(sizeof corners,
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                Memory::upload),
                     .block = PassBlock(pipelines, vertex.block_size())};
        std::memcpy(made.buffer.bytes().data(), corners.data(), sizeof corners);
        made.buffer.flush();
        const VkDeviceAddress address = made.buffer.address();
        for (const Field &field : vertex.fields())
          if (field.buffer())
            std::memcpy(
                made.block.bytes().data() + field.offset, &address, sizeof address);
        made.block.flush();
        return made;
      };
      const Corners middle = corners_at(0);
      const auto triangle = [&](const Offscreen &into, const Corners &at) {
        return Pass{.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
                    .pipeline = pipeline.handle(),
                    .block = at.block.set(),
                    .vertex_count = 3,
                    .offscreen = &into,
                    .reads = {at.buffer.handle()}};
      };
      const std::array cleared{Pass{.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .instance_count = 0,
                                    .offscreen = &sized}};
      const std::array unpiped{
          Pass{.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS, .offscreen = &sized}};
      const std::array unpiped_dispatch{Pass{.groups = 1}};
      const std::array one{triangle(sized, middle)};
      const std::array two{triangle(sized, middle), triangle(sized, middle)};
      const std::array none{triangle(unsized, middle)};
      rows.row("a clear alone -> " + frames.run({}, {}, cleared));
      rows.row("a draw with no pipeline but an instance -> " +
               frames.run({}, {}, unpiped));
      rows.row("a dispatch with no pipeline -> " + frames.run({}, {}, unpiped_dispatch));
      rows.row("the triangle -> " + frames.run({}, {}, one));
      rows.row("two triangles into one image, in one render pass -> " +
               frames.run({}, {}, two));
      // What the draws left, read back in the frame that drew it: a pass after the draws,
      // which draws grouped wrongly, or not drawn, would leave other pixels; a triangle
      // each side shows a second render pass clearing the first's.
      const std::filesystem::path snapshot = rows.repo() / "out" / "build" / "debug" /
                                             "views" / "library" / "vulpen" / "library" /
                                             "parts" / "snapshot" / "Snapshot.comp.spv";
      if (std::filesystem::exists(snapshot)) {
        const Picture picture(frames.engine, snapshot, drawn);
        const Corners left = corners_at(-0.5f);
        const Corners right = corners_at(0.5f);
        const std::array read{triangle(sized, middle), picture.pass};
        std::string ran = frames.run({}, {}, read);
        rows.row(std::format("the triangle, then its picture -> {}; {}",
                             ran,
                             picture.at({{32, 26}, {16, 26}, {48, 26}, {2, 60}})));
        const std::array beside{
            triangle(sized, left), triangle(sized, right), picture.pass};
        ran = frames.run({}, {}, beside);
        rows.row(std::format("a triangle each side, then their picture -> {}; {}",
                             ran,
                             picture.at({{16, 26}, {48, 26}, {32, 50}, {2, 60}})));
      } else {
        rows.row("no Snapshot.comp.spv: the debug build compiles it");
      }
      rows.row("the triangle into an image with no size yet -> " +
               frames.run({}, {}, none));
      // Its two timestamps are written back to back, as a pass with nothing to record
      // takes no time: a time all the same, not NaN, however long the GPU's thread was
      // kept between them.
      rows.row(std::format("times after it: {} pass, a time of at least 0: {}",
                           frames.engine.times().size(),
                           frames.engine.times().size() == 1 &&
                               frames.engine.times()[0] >= 0));
    });
  });
  carries_layer(bench, "Engine.cpp/Engine::run draws into an image");

  // Wave's dispatch, three frames: each glides the values a tenth of the way to the
  // target, and the CPU reads them back once the frame's fence passed (VK03).
  bench.check("Engine.cpp/Engine::run a dispatch", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const std::filesystem::path file = rows.repo() / "out" / "build" / "debug" /
                                         "views" / "wave" / "wave" / "Wave.comp.spv";
      if (!std::filesystem::exists(file)) {
        rows.row("no Wave.comp.spv: the debug build compiles it");
        return;
      }
      Frames frames(gpu.log);
      const Wave wave(frames.engine, file);
      const VkBuffer values = wave.values.handle();
      const std::array passes{wave.pass};
      for (std::uint64_t frame = 0; frame < 3; ++frame) {
        const std::string ran =
            frames.run(frame == 0 ? std::span(&values, 1) : std::span<const VkBuffer>{},
                       {},
                       passes,
                       frame);
        rows.row(
            std::format("frame {} -> {}; values {}", frame, ran, floats(wave.values, 4)));
      }
      // A dispatch takes time, unlike the triangle's empty pass, so its time is above 0,
      // which a mask of the timestamps' valid bits that drops them all would make 0.
      const std::span<const float> times = frames.engine.times();
      rows.row(std::format(
          "times: {} pass, above 0: {}", times.size(), !times.empty() && times[0] > 0));
    });
  });
  carries_layer(bench, "Engine.cpp/Engine::run a dispatch");

  // An engine made with a window, as a view that draws at start makes it: the triangle
  // drawn into the window each frame, after the passes into images; the window's output
  // closed, frames with none, and opened again.
  bench.check("Engine.cpp/Engine::run into the window", [](Rows &rows) {
    on_window(rows, [&](Gpu &gpu, const Window &window) {
      const std::filesystem::path folder =
          rows.repo() / "out" / "build" / "debug" / "views" / "triangle" / "triangle";
      if (!std::filesystem::exists(folder / "Triangle.vert.spv")) {
        rows.row("no Triangle.vert.spv: the debug build compiles it");
        return;
      }
      Engine engine(gpu.log, &window, "cpu");
      rows.row(std::format("made with a window: its render pass {}",
                           show(engine.render_pass(), rows.names())));
      const Shader vertex(folder / "Triangle.vert.spv");
      const Shader fragment(folder / "Triangle.frag.spv");
      const std::array stages{
          ShaderStage{.stage = VK_SHADER_STAGE_VERTEX_BIT, .shader = &vertex},
          ShaderStage{.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .shader = &fragment}};
      const Pipeline pipeline(engine.pipelines(), stages, engine.render_pass());
      constexpr std::array<float, 24> corners{-0.5f, -0.5f, 0, 1, 1, 0, 0, 1,
                                              0.5f,  -0.5f, 0, 1, 0, 1, 0, 1,
                                              0,     0.5f,  0, 1, 0, 0, 1, 1};
      const Buffer buffer = engine.resources().buffer(
          sizeof corners, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Memory::upload);
      std::memcpy(buffer.bytes().data(), corners.data(), sizeof corners);
      buffer.flush();
      const PassBlock block(engine.pipelines(), vertex.block_size());
      const VkDeviceAddress address = buffer.address();
      for (const Field &field : vertex.fields())
        if (field.buffer())
          std::memcpy(block.bytes().data() + field.offset, &address, sizeof address);
      block.flush();
      const std::array passes{Pass{.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   .pipeline = pipeline.handle(),
                                   .block = block.set(),
                                   .vertex_count = 3,
                                   .reads = {buffer.handle()}}};
      const auto frame = [&](std::uint64_t index) {
        try {
          engine.wait();
          engine.run({}, {}, passes, index, 0, {0, 0}, {});
          engine.wait();
          return std::string("done");
        } catch (...) {
          return thrown();
        }
      };
      for (std::uint64_t index = 0; index < 3; ++index)
        rows.row(std::format(
            "frame {}, the triangle into the window -> {}", index, frame(index)));
      engine.close();
      rows.row(std::format("closed: render pass {}; a frame -> {}",
                           show(engine.render_pass(), rows.names()),
                           frame(3)));
      engine.open(window);
      rows.row("opened again: a frame -> " + frame(4));
    });
  });
  carries_layer(bench, "Engine.cpp/Engine::run into the window");
  takes_the_main_thread(bench, "Engine.cpp/Engine::run into the window");
}

} // namespace VP::bench
