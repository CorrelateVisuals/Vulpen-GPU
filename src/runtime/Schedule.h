#pragma once

#include "baseclasses/Log.h"
#include "baseclasses/Resources.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

class CommandPort;
class FilePort;
class InputPort;
class Pipelines;
class Recipes;
class TerminalPort;
struct Connection;
struct Node;
struct Pass;
struct View;

// What every schedule borrows from the modules that own it (A01). Runtime.cpp fills it
// once, and whoever builds a schedule passes it on, so no builder includes those owners.
struct Wiring {
  const Pipelines &pipelines;
  const Resources &resources;
  // What draws render into; null without a window. Runtime.cpp sets it again whenever
  // the window opens or closes.
  VkRenderPass render_pass;
  Recipes &recipes;
  const Log &log;
  // The build tree's mirror of the views, where modules and SPIR-V land.
  const std::filesystem::path &views;
  CommandPort &commands;
  InputPort &input;
  FilePort &files;
  TerminalPort &terminal;
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
  // replaces, it takes what the build left alone: operators of recipes whose module
  // stayed, pipelines of unchanged SPIR-V, and buffers of unchanged shape, contents
  // included. The view must outlive it.
  Schedule(const Wiring &wiring, const View &view, Schedule *replaced = nullptr);
  ~Schedule();
  Schedule(const Schedule &) = delete;
  Schedule &operator=(const Schedule &) = delete;

  bool ok() const;
  // Before a module swap: the operators' code is about to go, so they go first.
  void drop_operators(const std::vector<std::string> &recipes);
  void cook(std::uint64_t frame);
  // The buffers made since the last call, which the next frame zeroes first.
  std::vector<VkBuffer> take_clears();
  const std::vector<Pass> &passes() const;

private:
  struct Bound;
  class Binder;
  class Cooker;

  Bound bind(const Node &node, const std::filesystem::path &folder, Schedule *replaced);
  void load_shaders(Bound &bound, const std::filesystem::path &folder, Bound *old);
  void make_pipeline(Bound &bound) const;
  void check_stages(Bound &bound) const;
  void check_fields(Bound &bound) const;
  void check_connections();
  void make_buffers(Schedule *replaced);
  void make_buffer(const Bound &writer,
                   const std::string &name,
                   VkDeviceSize size,
                   Memory memory,
                   Schedule *replaced);
  void make_blocks();
  void make_passes();
  void log(Level level, Tag tag, const Bound &bound, std::string_view text) const;
  Bound *find(std::string_view node);
  const Connection *connection_of(std::string_view node, std::string_view port) const;
  std::string buffer_name(std::string_view node, std::string_view port) const;

  const Wiring _wiring;
  const View &_view;
  std::vector<Bound> _bound; // in graph order
  std::map<std::string, Buffer, std::less<>> _buffers;
  std::vector<const Buffer *> _fresh;
  std::vector<Pass> _passes;
};

} // namespace VP
