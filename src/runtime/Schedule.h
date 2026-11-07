#pragma once

#include "baseclasses/Log.h"
#include "baseclasses/Resources.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VP {

class Commands;
class Engine;
class Modules;
class Pipelines;
class Ports;
struct Connection;
struct Copy;
struct Cost;
struct Node;
struct NodePort;
struct Offscreen;
struct Pass;
struct PixelFormat;
struct View;
// What a schedule holds of each node, of each object a connection between C++ nodes
// carries, and of each image a node's C++ filled, in runtime/Bound.h, which includes none
// of this header, so the schedule's own header and the files holding its parts, which
// include both, form no loop (A06), and this one includes none of what they hold (CPP13).
struct Bound;
struct Held;
struct Picture;

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
  // Where the view's draws into its window go: the window, or an image, while the view
  // that hosts it, or one hosting that, takes its window with a connection from
  // `<view>:`; or nowhere, while the view hosting it draws there itself. Its draws'
  // pipelines fit the one or the other, and drawing nowhere they have none.
  enum class Into : std::uint8_t { window, image, none };

  // Writers before their readers. Throws when the connections form a cycle.
  static std::vector<const Node *> order(const View &view);

  // Binds every node and checks each name that joins its manifest entry, shader and C++
  // (A02); a node with a mistake is left out, with its errors. From the schedule it
  // replaces, it takes what the build left alone: operators whose module stayed,
  // pipelines of unchanged SPIR-V that still fit what they draw into, and buffers of
  // unchanged shape and the images dispatches store into, contents included. The view
  // must outlive it. name: the view's, as a line addresses it, empty for the one vulpen
  // started with, which each line about one of its nodes keeps.
  // project: the view is a project, whose nodes' commands are its own, a line from it
  // reaching them first, so two projects' nodes of one name both register theirs; an
  // app's, as the cli's and the ide's, every view shares.
  Schedule(const Wiring &wiring,
           std::string_view name,
           const View &view,
           Into into,
           Schedule *replaced = nullptr,
           bool project = false);
  ~Schedule();
  Schedule(const Schedule &) = delete;
  Schedule &operator=(const Schedule &) = delete;

  bool ok() const;
  // Before a module swap: the operators' code is about to go, so they go first, with
  // their commands and the C++ objects their modules made. modules: as "view/folder".
  void drop_operators(const std::vector<std::string> &modules);
  // resolution: the window's size, or without one --size's, zero without either; the
  // images draws render into take it too.
  void cook(std::uint64_t frame, VkExtent2D resolution);
  // Whether the view rests, as nothing shows it (Views::pull): none of its nodes cooks,
  // and it hands on no work and no draws, so the passes are gathered again as it changes.
  void rest(bool rests);
  bool resting() const;
  // Its view closed in the middle of a frame, as by a line a node's cook runs: its nodes
  // cook no more and their commands go at once, as each reads its own view, gone now,
  // until the rebuild before the next frame drops the schedule.
  void retire();
  // The buffers made since the last call, which the next frame zeroes first.
  std::vector<VkBuffer> take_clears();
  // The images C++ filled since the last call, which the next frame copies in before
  // its passes; the buffers they copy from last until the call after.
  std::vector<Copy> take_copies();
  // Drops the pixels of the image a node's C++ fills, named by the port that fills it or
  // by a Texture that samples it, which then samples nothing until the node fills it
  // again. Throws when no node's C++ fills it.
  void clear_image(std::string_view node, std::string_view port);
  // The dispatches and the draws into images of their own, in graph order. Where each
  // lands among the frame's passes is kept, so its time finds its node; a frame gathers a
  // view's work before its draws, so the work starts that anew.
  void work(std::vector<Pass> &passes);
  // The draws into its window, in graph order, each recording into the image given, or
  // into the window when none is; whether it has any.
  bool draws(std::vector<Pass> &passes, const Offscreen *into);
  // A draw of nothing into the image, whose render pass clears it, for a window no
  // view's draw renders into.
  static void clears(std::vector<Pass> &passes, const Offscreen *into);
  // What each node costs, in graph order: its cook, as cook times it, and its pass, as
  // time hands it on.
  std::span<const Cost> costs() const;
  // Each node's ports, in graph order, as it bound them.
  std::span<const NodePort> ports() const;
  // Throws naming what its next bind would refuse of the params and connections of a
  // view, at nodes it bound whole: a param the node reads none of, or not as the value's
  // type, and a connection's end naming no port of the node, or one it only reads (A02).
  void check(const View &view) const;
  // How long each of the frame's passes took on the GPU, in milliseconds, by its place
  // among them, NaN for one not timed: read once the frame's fence passed, before
  // anything rebuilds, so the passes are the ones this schedule's work and draws gave.
  void time(std::span<const float> took);
  // The image the draws into the window of a view this one hosts render into, while a
  // connection takes it; null while none does.
  const Offscreen *window_of(std::string_view view) const;
  // Which of its nodes run: those the image named, node.port, needs, walking back along
  // the connections, as its view shows that image in its window's place (view show);
  // every node for none. Whether that changed, so the passes are gathered again.
  bool pull(std::string_view image);
  // How many of its nodes run, as pull left them.
  std::size_t running() const;
  // The slot of an image of its nodes, by node.port: one a draw renders into, a dispatch
  // stores into or C++ fills; 0 while it has none yet, and none for no such image.
  std::optional<std::uint32_t> image_slot(std::string_view image) const;
  // Its images by node.port, which a refusal names.
  std::vector<std::string> images() const;
  // The readers of the window of a view it takes, by `<view>:`, sample the image at the
  // slot instead, as that view shows an image in its window's place; none, their own.
  void show_instead(const std::string &window, std::optional<std::uint32_t> slot);

private:
  // Binder and Cooker are in runtime/Binder.cpp, so this header includes none of what
  // they hold (CPP13).
  struct Rendered; // in runtime/Schedule.cpp, the one file that reaches its parts
  struct Stored;   // so is this
  class Binder;
  class Cooker;
  // Each port a connection joins, by its node and port: the first connection naming it.
  using Joined =
      std::map<std::pair<std::string_view, std::string_view>, const Connection *>;

  static Joined joined_of(const View &view);

  Bound bind(const Node &node, const std::filesystem::path &folder, Schedule *replaced);
  void load_shaders(Bound &bound, const std::filesystem::path &folder, Bound *old);
  bool check_counts(Bound &bound) const;
  std::uint32_t counted(Bound &bound) const;
  void read_shaders(Bound &bound) const;
  void fit_pipeline(Bound &bound, Bound *old, bool unchanged);
  VkRenderPass target_of(Bound &bound) const;
  void make_pipeline(Bound &bound, VkRenderPass render_pass) const;
  void pair_draws(Schedule *replaced);
  Bound *fragment_of(Bound &vertex);
  void check_varyings(const Bound &vertex,
                      Bound &fragment,
                      const std::vector<const Connection *> &varyings) const;
  void check_stages(Bound &bound) const;
  void check_shaders(const Bound &bound) const;
  void check_fields(Bound &bound) const;
  void check_connections();
  void check_inputs(const Connection &connection);
  void check_images();
  void list_ports();
  void keep_images(Schedule &replaced);
  void keep_stored(Schedule &replaced);
  void make_buffers(Schedule *replaced);
  void make_buffer(Bound &writer,
                   const std::string &name,
                   std::uint32_t elements,
                   std::uint32_t stride,
                   Memory memory,
                   Schedule *replaced);
  void make_blocks();
  void make_targets();
  void size_targets(VkExtent2D size);
  void size_stored(Stored &stored, VkExtent2D size);
  void make_passes();
  void cost_from(const Schedule *replaced);
  void upload(const Bound &writer,
              std::uint32_t texture,
              std::span<const std::byte> pixels,
              VkExtent2D extent);
  const Image &image_for(const Bound &writer,
                         const std::string &name,
                         VkExtent2D extent,
                         const PixelFormat &format);
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
  const std::string _name;
  const View &_view;
  // Found once a rebuild, as the lookups of every node and field search them (C03).
  const Joined _joined;
  std::map<std::string_view, std::size_t> _places; // in _bound, by each node's name
  // What its draws into its window record in; null while that is a window none opened,
  // or while they draw nowhere.
  const VkRenderPass _window;
  const Into _into;
  const bool _project;
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
  // That draws render into, its own and the windows of views it hosts; each stays where
  // it is, as the passes that draw it point to it.
  std::vector<std::unique_ptr<Rendered>> _rendered;
  std::vector<Stored> _stored; // that dispatches store into
  std::vector<Copy> _copies;
  // What the copies read: this frame's, and those of the frame the GPU may still run.
  std::vector<Buffer> _staged;
  std::vector<Buffer> _in_flight;
  std::vector<Pass> _passes;
  std::vector<std::size_t> _passed; // by _passes, its node's place in _bound
  std::vector<std::size_t> _landed; // by _passes, its place among the frame's
  std::vector<Cost> _costs;         // by _bound
  std::vector<NodePort> _ports;     // by _bound, each node's in turn
  bool _retired = false;
  bool _resting = false;
  std::vector<bool> _pulled; // by _bound, those that run; empty while every node does
  std::string _pulled_for;   // the image they are what is needed of
  // The windows whose readers sample another image, by `<view>:`, and its slot.
  std::map<std::string, std::uint32_t, std::less<>> _instead;
};

} // namespace VP
