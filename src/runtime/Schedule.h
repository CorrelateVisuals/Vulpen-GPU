#pragma once

#include "baseclasses/Log.h"
#include "baseclasses/Resources.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

class Commands;
class Engine;
class Modules;
class Pipelines;
class Ports;
struct Connection;
struct Copy;
struct Node;
struct Pass;
struct View;

// What every schedule borrows from the modules that own it (A01). Runtime.cpp fills it
// once, and whoever builds a schedule passes it on, so no builder includes those owners.
struct Wiring {
  const Engine &engine; // which a node may include by hand (native C++)
  const Pipelines &pipelines;
  const Resources &resources;
  // What draws render into; null without a window. Runtime.cpp sets it again whenever
  // the window opens or closes.
  VkRenderPass render_pass;
  Modules &modules;
  const Log &log;
  // The build tree's mirror of the views, where modules and SPIR-V land.
  const std::filesystem::path &views;
  Commands &commands;
  Ports &ports;
};

// Where the graph meets the GPU: runs each node's operator in graph order and turns
// the view into passes that declare what they read and write.
class Schedule {
public:
  // Writers before their readers. Throws when the connections form a cycle.
  static std::vector<const Node *> order(const View &view);
  // Whether a node of the view draws, so the view needs a window to draw into.
  static bool draws(const View &view);

  // Binds every node and checks each name that joins its manifest entry, shader and C++
  // (A02); a node with a mistake is left out, with its errors. From the schedule it
  // replaces, it takes what the build left alone: operators whose module stayed,
  // pipelines of unchanged SPIR-V, and buffers of unchanged shape, contents included. The
  // view must outlive it.
  Schedule(const Wiring &wiring, const View &view, Schedule *replaced = nullptr);
  ~Schedule();
  Schedule(const Schedule &) = delete;
  Schedule &operator=(const Schedule &) = delete;

  bool ok() const;
  // Before a module swap: the operators' code is about to go, so they go first, with
  // their commands and the C++ objects their modules made. modules: as "view/folder".
  void drop_operators(const std::vector<std::string> &modules);
  void cook(std::uint64_t frame);
  // The buffers made since the last call, which the next frame zeroes first.
  std::vector<VkBuffer> take_clears();
  // The images C++ filled since the last call, which the next frame copies in before
  // its passes; the buffers they copy from last until the call after.
  std::vector<Copy> take_copies();
  const std::vector<Pass> &passes() const;

private:
  // Bound, Held and Picture are in runtime/Bound.h, and Binder and Cooker in
  // runtime/Binder.cpp, so this header includes none of what they hold (CPP13).
  struct Bound;
  struct Held;
  struct Picture;
  class Binder;
  class Cooker;

  Bound bind(const Node &node, const std::filesystem::path &folder, Schedule *replaced);
  void load_shaders(Bound &bound, const std::filesystem::path &folder, Bound *old);
  bool check_counts(Bound &bound) const;
  void make_pipeline(Bound &bound) const;
  void check_stages(Bound &bound) const;
  void check_fields(Bound &bound) const;
  void check_connections();
  void check_inputs(const Connection &connection);
  void check_images();
  void keep_images(Schedule &replaced);
  void make_buffers(Schedule *replaced);
  void make_buffer(const Bound &writer,
                   const std::string &name,
                   std::uint32_t elements,
                   std::uint32_t stride,
                   Memory memory,
                   Schedule *replaced);
  void make_blocks();
  void make_passes();
  void upload(const Bound &writer,
              std::string_view port,
              std::span<const std::byte> pixels,
              VkExtent2D extent);
  const Image &image_for(const Bound &writer, const std::string &name, VkExtent2D extent);
  void point_readers(const std::string &name, std::uint32_t slot);
  void drop_commands(Bound &bound);
  void close_files(Bound &bound);
  void log(Level level, Tag tag, const Bound &bound, std::string_view text) const;
  Bound *find(std::string_view node);
  const Held *held(std::string_view name) const;
  const Connection *connection_of(std::string_view node, std::string_view port) const;
  std::string buffer_name(std::string_view node, std::string_view port) const;
  std::string image_name(std::string_view node, std::string_view port) const;

  const Wiring _wiring;
  const View &_view;
  // What connections between C++ nodes carry, which outlives the operators that hold
  // references to it.
  std::vector<Held> _objects;
  std::vector<Bound> _bound; // in graph order
  std::map<std::string, Buffer, std::less<>> _buffers;
  // Each buffer's used length in elements, which a draw's instance count may follow; a
  // pass holds its address, which stays while the schedule does.
  std::map<std::string, std::uint32_t, std::less<>> _used;
  std::vector<const Buffer *> _fresh;
  std::vector<Picture> _images; // that nodes' C++ filled
  std::vector<Copy> _copies;
  // What the copies read: this frame's, and those of the frame the GPU may still run.
  std::vector<Buffer> _staged;
  std::vector<Buffer> _in_flight;
  std::vector<Pass> _passes;
};

} // namespace VP
