#include "baseclasses/Pipelines.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// baseclasses/Pipelines.cpp: the one pipeline layout, set 0's slots, the frame block, the
// pass blocks' pools, and the pipelines of every shader the debug build compiles, each
// stage set as a node's folder holds it, on a device on lavapipe (bench/Gpu.h). The
// fail-loud test's shaders are worlds too: what a pipeline of them does is a row each.

namespace VP::bench {

struct PipelinesFrame {
  friend constexpr auto reach(PipelinesFrame);
};
template struct Reached<PipelinesFrame, &VP::Pipelines::_frame>;
struct PipelinesPools {
  friend constexpr auto reach(PipelinesPools);
};
template struct Reached<PipelinesPools, &VP::Pipelines::_pools>;
// Spelt out, though Reached could deduce it, as MSVC calls what a lambda reads through a
// deduced one void.
struct PipelinesFree {
  using type = std::vector<std::uint32_t> VP::Pipelines::*;
  friend constexpr type reach(PipelinesFree);
};
template struct Reach<PipelinesFree, &VP::Pipelines::_free>;
struct ShaderFrame {
  friend constexpr auto reach(ShaderFrame);
};
template struct Reached<ShaderFrame, &VP::Shader::_frame>;

namespace domain {
// Where the pointer is, in pixels from the top left.
struct Cursor {};
} // namespace domain

template <> struct Gen<domain::Cursor> {
  using type = std::array<float, 2>;
  static std::vector<type> edges() {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    return {{0.5f, 640.25f},
            {0, 0},
            {-0.0f, 0},
            {-1, -1},
            {nan, nan},
            {inf, 0},
            {1e9f, 1e9f}};
  }
  static type random(Random &random) {
    return {static_cast<float>(random.below(4096)) / 4,
            static_cast<float>(random.below(4096)) / 4};
  }
};

template <> struct Shown<std::array<float, 2>> {
  static std::string text(const std::array<float, 2> &value, Names &) {
    return std::format("({},{})", number_text(value[0]), number_text(value[1]));
  }
};

namespace {

std::filesystem::path build_folder(const Rows &rows) {
  return rows.repo() / "out" / "build" / "debug";
}

// A node's stage files, as its folder holds them: by folder and name, each stage's file.
using StageSets = std::map<std::string, std::map<std::string, std::filesystem::path>>;

StageSets stage_sets(const Rows &rows) {
  StageSets sets;
  const std::filesystem::path build = build_folder(rows);
  if (!std::filesystem::exists(build))
    return sets;
  // Only the views the build lists (nodes.cmake): a project removed since leaves its
  // shaders in the folder, which a fresh build has not.
  std::set<std::string> views;
  std::istringstream listed(Files::text(build / VP_VIEWS_FILE));
  for (std::string line; std::getline(listed, line);)
    views.insert(line);
  for (const auto &entry : std::filesystem::recursive_directory_iterator(build)) {
    const std::filesystem::path relative = entry.path().lexically_relative(build);
    if (!entry.is_regular_file() || entry.path().extension() != ".spv" ||
        relative.generic_string().starts_with("bench/") ||
        (*relative.begin() == "views" &&
         !views.contains(std::next(relative.begin())->string())))
      continue;
    const std::filesystem::path stem = relative.stem(); // Name.comp
    const std::string set = (relative.parent_path() / stem.stem()).generic_string();
    sets[set][stem.extension().string().substr(1)] = entry.path();
  }
  return sets;
}

// The stages of a draw in the order its data goes through them (Pipelines.h).
constexpr std::array<std::pair<std::string_view, VkShaderStageFlagBits>, 5> draw_stages{
    {{"vert", VK_SHADER_STAGE_VERTEX_BIT},
     {"tesc", VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT},
     {"tese", VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT},
     {"geom", VK_SHADER_STAGE_GEOMETRY_BIT},
     {"frag", VK_SHADER_STAGE_FRAGMENT_BIT}}};

template <class F> std::string said(F &&call) {
  try {
    call();
    return "made";
  } catch (...) {
    return thrown();
  }
}

// The frame block as the GPU reads it, each member at the offset reflection gave.
std::string frame_bytes(const Pipelines &pipelines) {
  const auto &frame = pipelines.*reach(PipelinesFrame{});
  if (!frame)
    return "no frame block";
  const std::byte *const bytes = frame->buffer.bytes().data();
  const auto at = [&](std::size_t member, auto value) {
    std::memcpy(&value, bytes + frame->fields[member].offset, sizeof value);
    return value;
  };
  const auto size = at(FrameMember::resolution, std::array<std::uint32_t, 2>{});
  const auto cursor = at(FrameMember::cursor, std::array<float, 2>{});
  return std::format("resolution ({},{}) cursor ({},{}) time {} index {}",
                     size[0],
                     size[1],
                     number_text(cursor[0]),
                     number_text(cursor[1]),
                     number_text(at(FrameMember::time, 0.0f)),
                     at(FrameMember::frame_index, std::uint32_t{}));
}

// A compute shader of the build that declares the frame block, for a pipeline that makes
// it.
std::optional<std::filesystem::path> frame_shader(const Rows &rows) {
  for (const auto &[set, stages] : stage_sets(rows))
    if (const auto compute = stages.find("comp");
        compute != stages.end() && !set.starts_with("views/mistakes/") &&
        !Shader(compute->second).frame().empty())
      return compute->second;
  return std::nullopt;
}

std::string pools_text(const Pipelines &pipelines) {
  std::string text;
  for (const auto &pool : pipelines.*reach(PipelinesPools{}))
    text += std::format("{}{}", text.empty() ? "" : ",", pool.blocks);
  return "[" + text + "]";
}

} // namespace

void cases(Bench &bench) {
  // Every stage set of the build: a dispatch's pipeline for a .comp, a draw's into an
  // rgba8 image's render pass for one with a .vert, its stages in their order.
  bench.check("Pipelines.cpp/Pipeline every shader of the build", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      const StageSets sets = stage_sets(rows);
      if (sets.empty())
        rows.row("no shader: the debug build compiles them");
      for (const auto &[set, stages] : sets) {
        if (const auto compute = stages.find("comp"); compute != stages.end())
          rows.row(std::format("{} comp -> {}", set, said([&] {
                                 const Shader shader(compute->second);
                                 const Pipeline pipeline(pipelines, shader);
                               })));
        if (!stages.contains("vert"))
          continue;
        std::string names;
        rows.row(std::format(
            "{} draw -> {}", set, said([&] {
              std::vector<Shader> shaders;
              std::vector<ShaderStage> drawn;
              for (const auto &[stage, flag] : draw_stages)
                if (const auto file = stages.find(std::string(stage));
                    file != stages.end())
                  shaders.emplace_back(file->second);
              std::size_t index = 0;
              for (const auto &[stage, flag] : draw_stages)
                if (stages.contains(std::string(stage)))
                  drawn.push_back({.stage = flag, .shader = &shaders[index++]});
              const Pipeline pipeline(
                  pipelines, drawn, pipelines.render_pass(VK_FORMAT_R8G8B8A8_UNORM));
            })));
      }
      rows.row(std::format("the frame block after them: address {}",
                           pipelines.frame() != 0 ? "set" : "none"));
    });
  });
  carries_layer(bench, "Pipelines.cpp/Pipeline every shader of the build");

  // The first shader to declare the frame block declares the one the engine writes, each
  // member by its name and its type: a shader of the build, its time member named or
  // typed otherwise in what reflection read of it.
  bench.check("Pipelines.cpp/Pipelines::take_frame, a block declared otherwise",
              [](Rows &rows) {
                on_gpu(rows, [&](Gpu &gpu) {
                  const std::optional<std::filesystem::path> file = frame_shader(rows);
                  if (!file) {
                    rows.row("no shader of the build declares the frame block");
                    return;
                  }
                  constexpr std::array<std::array<std::string_view, 3>, 3> times{
                      {{"time as declared", "time", "float"},
                       {"time named tame", "tame", "float"},
                       {"time a uint", "time", "uint"}}};
                  for (const auto &[what, name, type] : times) {
                    const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
                    Shader shader(*file);
                    Field &time = (shader.*reach(ShaderFrame{})).at(FrameMember::time);
                    time.name = name;
                    time.type = type;
                    rows.row(std::format("{} -> {}", what, said([&] {
                                           const Pipeline pipeline(pipelines, shader);
                                         })));
                  }
                });
              });

  // What every pass reads of the frame: the bytes write_frame leaves, read back at the
  // offsets reflection gave, once a shader has declared the block.
  bench.check("Pipelines.cpp/Pipelines::write_frame", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      rows.row("before any shader declares the block -> " +
               said([&] { pipelines.write_frame(1, 1, {1, 1}, {1, 1}); }) + "; " +
               frame_bytes(pipelines));
      const std::optional<std::filesystem::path> file = frame_shader(rows);
      if (!file) {
        rows.row("no shader of the build declares the frame block");
        return;
      }
      const Shader shader(*file);
      const Pipeline pipeline(pipelines, shader);
      sweep_in(
          rows,
          "Pipelines.cpp/Pipelines::write_frame",
          pipelines,
          [](const Pipelines &on,
             std::uint64_t index,
             double time,
             VkExtent2D resolution,
             std::array<float, 2> cursor) {
            on.write_frame(index, time, resolution, cursor);
            return frame_bytes(on);
          },
          domain::Type{},
          domain::Type{},
          domain::Type{},
          domain::Cursor{});
    });
  });

  bench.check("Pipelines.cpp/Pipelines::render_pass", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      sweep_in(rows,
               "Pipelines.cpp/Pipelines::render_pass",
               pipelines,
               [&rows](const Pipelines &on, VkFormat format) {
                 const VkRenderPass made = on.render_pass(format);
                 return std::format("{}; asked again, the same: {}",
                                    show(made, rows.names()),
                                    on.render_pass(format) == made);
               });
    });
  });
  carries_layer(bench, "Pipelines.cpp/Pipelines::render_pass");

  // A target drawn with a depth: none, one dropped once the pass ends, and one kept for
  // the passes after it to sample. Each its own render pass, the same when asked again;
  // a Drawn of each, moved, with its kept depth's slot and size.
  bench.check("Pipelines.cpp/Drawn, with depth", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      Names &names = rows.names();
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      constexpr std::array<std::string_view, 3> words{"none", "dropped", "kept"};
      for (const Depth depth : {Depth::none, Depth::dropped, Depth::kept}) {
        const std::string_view word = words.at(static_cast<std::size_t>(depth));
        const VkRenderPass pass = pipelines.render_pass(VK_FORMAT_R8G8B8A8_UNORM, depth);
        rows.row(
            std::format("render pass, depth {} -> {}; asked again, the same: {}",
                        word,
                        show(pass, names),
                        pipelines.render_pass(VK_FORMAT_R8G8B8A8_UNORM, depth) == pass));
        std::string drawn_text;
        try {
          Drawn drawn(pipelines,
                      gpu.resources.image({64, 32}, VK_FORMAT_R8G8B8A8_UNORM, Fill::draw),
                      depth);
          const Drawn moved(std::move(drawn));
          const Sampled *const kept = moved.depth();
          drawn_text = std::format("a framebuffer {}, the old one's {}; kept depth {}",
                                   moved.framebuffer() != VK_NULL_HANDLE,
                                   drawn.framebuffer() != VK_NULL_HANDLE,
                                   kept ? std::format("slot {}, {}x{}",
                                                      kept->slot(),
                                                      kept->image().extent().width,
                                                      kept->image().extent().height)
                                        : std::string("none"));
        } catch (...) {
          drawn_text = thrown();
        }
        rows.row(std::format("drawn, depth {} -> {}", word, drawn_text));
      }
    });
  });
  carries_layer(bench, "Pipelines.cpp/Drawn, with depth");

  // A pass block of each size a shader may declare, and past what a uniform block holds.
  bench.check("Pipelines.cpp/PassBlock", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      for (const std::uint32_t size :
           {256u, 16u, 4u, 0u, 16384u, 65536u, 65537u, 1u << 20})
        rows.row(std::format("PassBlock of {} bytes -> {}", size, [&] {
          try {
            const PassBlock block(pipelines, size);
            return std::format("set {}, {} bytes mapped",
                               show(block.set(), rows.names()),
                               block.bytes().size());
          } catch (...) {
            return thrown();
          }
        }()));
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(gpu.mechanics.physical_device(), &properties);
      rows.row(std::format("this GPU's maxUniformBufferRange -> {}",
                           properties.limits.maxUniformBufferRange));
    });
  });
  carries_layer(bench, "Pipelines.cpp/PassBlock");

  // Blocks counted into pools of 1,024: a block past a full pool makes the next pool,
  // and one that goes counts itself out.
  bench.check("Pipelines.cpp/Pipelines::pool", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      std::vector<PassBlock> blocks;
      for (int index = 0; index < 1025; ++index)
        blocks.emplace_back(pipelines, 16);
      rows.row("1,025 blocks -> pools " + pools_text(pipelines));
      blocks.erase(blocks.begin(), blocks.begin() + 1000);
      rows.row("1,000 of them gone -> pools " + pools_text(pipelines));
      for (int index = 0; index < 1000; ++index)
        blocks.emplace_back(pipelines, 16);
      rows.row("1,000 more -> pools " + pools_text(pipelines));
      blocks.clear();
      rows.row("all gone -> pools " + pools_text(pipelines));
    });
  });

  // textures[]'s slots: the lowest free first, each image's own while it lives, none past
  // the last, and a slot that is freed is the next one taken.
  bench.check("Pipelines.cpp/Pipelines::take_slot", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      const auto image = [&](Fill fill) {
        return gpu.resources.image({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, fill);
      };
      std::vector<Sampled> taken;
      taken.emplace_back(pipelines, image(Fill::copy));
      taken.emplace_back(pipelines, image(Fill::store));
      taken.emplace_back(pipelines, image(Fill::draw));
      rows.row(std::format("the first three take slots {}, {}, {}",
                           taken[0].slot(),
                           taken[1].slot(),
                           taken[2].slot()));
      taken.erase(taken.begin() + 1);
      const Sampled next(pipelines, image(Fill::copy));
      rows.row(std::format("slot 2 freed, the next takes {}", next.slot()));
      std::string last;
      while (taken.size() < 2000) {
        try {
          taken.emplace_back(pipelines, image(Fill::copy));
        } catch (...) {
          last = thrown();
          break;
        }
      }
      rows.row(
          std::format("taken until refused: {} more held, then {}", taken.size(), last));
      rows.row(std::format("free slots then -> {}",
                           (pipelines.*reach(PipelinesFree{})).size()));
      taken.clear();
      rows.row(
          std::format("all gone -> {} free", (pipelines.*reach(PipelinesFree{})).size()));
    });
  });

  // One owner through every move, as Resources' handles (A01): a moved-from object holds
  // nothing, a slot or a set goes back once, and a framebuffer goes with its image.
  bench.check("Pipelines.cpp/moves", [](Rows &rows) {
    on_gpu(rows, [&](Gpu &gpu) {
      const Pipelines pipelines(gpu.mechanics.device(), gpu.resources);
      const auto free_slots = [&] { return (pipelines.*reach(PipelinesFree{})).size(); };
      const std::size_t before = free_slots();
      {
        Sampled first(pipelines, gpu.resources.image({4, 4}, VK_FORMAT_R8G8B8A8_UNORM));
        Sampled second(std::move(first));
        Sampled third(pipelines, gpu.resources.image({4, 4}, VK_FORMAT_R8_UNORM));
        third = std::move(second);
        Sampled &same = third;
        third = std::move(same);
        rows.row(std::format("Sampled moved: the old one's slot {}, the kept one's {}",
                             first.slot(),
                             third.slot()));
      }
      rows.row(std::format("Sampled all gone: free slots as before: {}",
                           free_slots() == before));
      {
        Drawn first(pipelines,
                    gpu.resources.image({4, 4}, VK_FORMAT_R8G8B8A8_UNORM, Fill::draw));
        Drawn second(std::move(first));
        Drawn third(pipelines,
                    gpu.resources.image({8, 8}, VK_FORMAT_R8G8B8A8_SRGB, Fill::draw));
        third = std::move(second);
        rows.row(
            std::format("Drawn moved: the old one's framebuffer {}, the kept one's {}",
                        show(first.framebuffer(), rows.names()),
                        show(third.framebuffer(), rows.names())));
      }
      rows.row(std::format("Drawn all gone: free slots as before: {}",
                           free_slots() == before));
      {
        PassBlock first(pipelines, 64);
        PassBlock second(std::move(first));
        PassBlock third(pipelines, 32);
        third = std::move(second);
        rows.row(
            std::format("PassBlock moved: the old one's set {}, the kept one's {} bytes",
                        show(first.set(), rows.names()),
                        third.bytes().size()));
      }
      rows.row("PassBlock all gone -> pools " + pools_text(pipelines));
    });
  });
}

} // namespace VP::bench
