#include "runtime/Schedule.h"

#include "baseclasses/Passes.h"
#include "runtime/Bound.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace VP {

namespace {

// What Cook::upload hands over: one byte a pixel, read as 0 to 1.
constexpr VkFormat pixel_format = VK_FORMAT_R8_UNORM;

// The files of the node's folder that are shaders, by their extension.
std::vector<std::string> shaders_of(const Node &node) {
  std::vector<std::string> shaders;
  for (const std::string &file : node.files)
    if (std::ranges::any_of(
            stages, [&](std::string_view stage) { return file.ends_with(stage); }))
      shaders.push_back(file);
  return shaders;
}

// A buffer holds one element per invocation of its writer: a dispatch's threads, or a
// draw's vertices.
std::uint32_t invocations_of(const Node &node) {
  return is_draw(node) ? node.vertex_count : node.invocations;
}

// By Access and by Memory, in the words a log line uses.
constexpr std::array access_words{
    std::string_view{"readonly "}, std::string_view{"writeonly "}, std::string_view{}};
constexpr std::array memory_words{std::string_view{"on the GPU"},
                                  std::string_view{"written by the CPU"},
                                  std::string_view{"read back by the CPU"}};

// A pass block as reflection finds it, each field declared as GLSL would.
std::string pass_block(const std::vector<Field> &fields, std::uint32_t size) {
  std::string text = std::format("pass block of {} bytes:", size);
  for (const Field &field : fields)
    text += std::format(
        "{} {}{}{} {} at {}",
        &field == &fields.front() ? "" : ",",
        field.buffer() ? access_words[static_cast<std::size_t>(field.access)] : "",
        field.type,
        field.buffer() ? "[]" : "",
        field.name,
        field.offset);
  return text;
}

} // namespace

// Of the nodes whose writers are all placed, the first in the manifest goes next, so
// nodes the connections leave unordered, such as two draws, run as the manifest lists
// them. Every rebuild reorders the view, so the connections are indexed once, not
// searched for each node placed.
std::vector<const Node *> Schedule::order(const View &view) {
  std::map<std::string_view, std::size_t> index; // into view.nodes, by name
  for (const Node &node : view.nodes)
    index.emplace(node.name, index.size());
  std::vector<std::size_t> waits(view.nodes.size()); // writers not placed yet
  std::multimap<std::size_t, std::size_t> readers;
  for (const Connection &connection : view.connections)
    for (const Endpoint &to : connection.to)
      if (to.node != connection.from.node) {
        readers.emplace(index.at(connection.from.node), index.at(to.node));
        ++waits[index.at(to.node)];
      }
  std::set<std::size_t> ready;
  for (std::size_t node = 0; node < waits.size(); ++node)
    if (waits[node] == 0)
      ready.insert(node);
  std::vector<const Node *> placed;
  while (!ready.empty()) {
    const std::size_t node = ready.extract(ready.begin()).value();
    placed.push_back(&view.nodes[node]);
    for (auto [reader, end] = readers.equal_range(node); reader != end; ++reader)
      if (--waits[reader->second] == 0)
        ready.insert(reader->second);
  }
  if (placed.size() < view.nodes.size()) {
    const auto left =
        std::ranges::find_if(waits, [](std::size_t count) { return count != 0; });
    throw std::runtime_error(std::format("{}: the connections form a cycle through {}",
                                         view.file.string(),
                                         view.nodes[left - waits.begin()].name));
  }
  return placed;
}

bool Schedule::draws(const View &view) {
  return std::ranges::any_of(view.nodes, is_draw);
}

Schedule::Schedule(const Wiring &wiring, const View &view, Schedule *replaced)
    : _wiring(wiring), _view(view) {
  const std::vector<const Node *> nodes = order(view);
  // Every node registers its commands again as it binds, so no command stays behind
  // with a node that went, and none blocks a node that takes its name.
  if (replaced)
    for (Bound &bound : replaced->_bound)
      replaced->drop_commands(bound);
  _bound.reserve(nodes.size());
  for (const Node *const node : nodes)
    _bound.push_back(bind(*node, wiring.views / view.name / node->module, replaced));
  check_connections();
  check_images();
  make_buffers(replaced);
  if (replaced)
    keep_images(*replaced);
  make_blocks();
  make_passes();
  // Each error names the line that last added or changed the node, if a file holds it,
  // else the view's file, in its folder as the lines are. A node left out answers no
  // command either.
  const std::string file =
      (view.file.parent_path().filename() / view.file.filename()).generic_string();
  for (Bound &bound : _bound) {
    if (!bound.errors.empty())
      drop_commands(bound);
    for (const std::string &error : bound.errors)
      _wiring.log.write(Level::error,
                        Tag::nod,
                        std::format("{} node {}: {}",
                                    bound.node->where.empty() ? file : bound.node->where,
                                    bound.node->name,
                                    error));
  }
}

// A node's files outlive its code, so they go only with the schedule, once the next one
// opened them again.
Schedule::~Schedule() {
  for (Bound &bound : _bound) {
    drop_commands(bound);
    close_files(bound);
  }
}

// A node whose SPIR-V the build did not rewrite keeps its reflection and pipeline.
void Schedule::load_shaders(Bound &bound,
                            const std::filesystem::path &folder,
                            Bound *old) {
  const std::vector<std::string> &shaders = bound.shaders_named = shaders_of(*bound.node);
  if (!check_counts(bound))
    return;
  for (const std::string &shader : shaders) {
    std::error_code missing;
    bound.spirv.push_back(folder / (shader + ".spv"));
    bound.built.push_back(std::filesystem::last_write_time(bound.spirv.back(), missing));
    if (missing) {
      bound.errors.push_back(std::format("{} is not built: the build compiles it to {}",
                                         shader,
                                         bound.spirv.back().string()));
      return;
    }
  }
  if (old && old->pipeline && old->spirv == bound.spirv && old->built == bound.built) {
    bound.shaders = std::move(old->shaders);
    bound.fields = std::move(old->fields);
    bound.block_size = old->block_size;
    bound.pipeline = std::move(old->pipeline);
    log(Level::debug, Tag::nod, bound, "keeps its pipeline");
  } else {
    try {
      make_pipeline(bound);
    } catch (const std::exception &failure) {
      bound.shaders.clear();
      bound.fields.clear();
      bound.errors.emplace_back(failure.what());
      return;
    }
  }
  check_stages(bound);
}

// A node's shaders share its one pass block, so they must agree on where each field is.
void Schedule::make_pipeline(Bound &bound) const {
  const Node &node = *bound.node;
  for (const std::filesystem::path &spirv : bound.spirv)
    bound.shaders.emplace_back(spirv);
  for (const Shader &shader : bound.shaders) {
    if (shader.fields().empty())
      continue;
    if (bound.fields.empty()) {
      bound.fields = shader.fields();
      bound.block_size = shader.block_size();
    } else if (bound.fields != shader.fields()) {
      throw std::runtime_error("its shaders declare different pass blocks; declare it "
                               "once, in a file both include");
    }
  }
  if (!is_draw(node)) {
    bound.pipeline.emplace(_wiring.pipelines, bound.shaders.front());
  } else if (!_wiring.render_pass) {
    throw std::runtime_error("it draws, but no window is open to draw into");
  } else {
    const std::size_t vertex =
        bound.shaders_named.front().ends_with(vertex_stage) ? 0 : 1;
    bound.pipeline.emplace(_wiring.pipelines,
                           bound.shaders[vertex],
                           bound.shaders[1 - vertex],
                           _wiring.render_pass);
  }
  log(Level::info, Tag::nod, bound, "pipeline from " + joined(bound.shaders_named));
  if (!bound.fields.empty())
    log(Level::debug, Tag::nod, bound, pass_block(bound.fields, bound.block_size));
}

// One buffer per written port, shared through its connection. A buffer the CPU writes
// or reads back lives where the CPU can map it.
void Schedule::make_buffers(Schedule *replaced) {
  std::set<std::string, std::less<>> read_back;
  for (const Bound &bound : _bound)
    for (const std::string &port : bound.readbacks)
      read_back.insert(buffer_name(bound.node->name, port));
  for (const Bound &bound : _bound)
    for (const Field &field : bound.fields) {
      const Bound::Written *const upload = bound.upload(field.name);
      if (!field.buffer() || (!writes(field.access) && !upload))
        continue;
      const std::string name = buffer_name(bound.node->name, field.name);
      const Memory memory = upload                     ? Memory::upload
                            : read_back.contains(name) ? Memory::readback
                                                       : Memory::device;
      const std::uint32_t elements =
          std::max(invocations_of(*bound.node), upload ? upload->count : 0);
      make_buffer(bound, name, elements, field.stride, memory, replaced);
    }
  const auto buffers_of = [&](const Bound &bound, const std::vector<std::string> &ports) {
    std::vector<const Buffer *> buffers;
    for (const std::string &port : ports) {
      const auto found = _buffers.find(buffer_name(bound.node->name, port));
      buffers.push_back(found == _buffers.end() ? nullptr : &found->second);
    }
    return buffers;
  };
  for (Bound &bound : _bound) {
    bound.readback_buffers = buffers_of(bound, bound.readbacks);
    for (Bound::Written &upload : bound.uploads) {
      const std::string name = buffer_name(bound.node->name, upload.port);
      const auto found = _buffers.find(name);
      upload.buffer = found == _buffers.end() ? nullptr : &found->second;
      upload.used = &_used[name];
    }
  }
}

// The node that writes the buffer speaks for it, at that node's log level (V09).
void Schedule::make_buffer(const Bound &writer,
                           const std::string &name,
                           std::uint32_t elements,
                           std::uint32_t stride,
                           Memory memory,
                           Schedule *replaced) {
  if (_buffers.contains(name))
    return;
  const VkDeviceSize size = VkDeviceSize{elements} * stride;
  _used[name] = elements; // until C++ writes fewer
  if (replaced) {
    auto kept = replaced->_buffers.extract(name);
    if (kept && kept.mapped().size() == size && kept.mapped().memory() == memory) {
      // Edits rebuild with no frame between, so one no frame has zeroed yet stays fresh.
      if (std::ranges::find(replaced->_fresh, &kept.mapped()) != replaced->_fresh.end())
        _fresh.push_back(&kept.mapped());
      _buffers.insert(std::move(kept));
      if (const auto used = replaced->_used.find(name); used != replaced->_used.end())
        _used[name] = used->second;
      log(Level::debug, Tag::mem, writer, "keeps " + name + " and what it holds");
      return;
    }
  }
  Buffer made = _wiring.resources.buffer(
      size,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      memory);
  // The operator writes an upload buffer before the frame whose clears would zero it,
  // so the CPU zeroes it instead (C01).
  if (memory == Memory::upload) {
    std::ranges::fill(made.bytes(), std::byte{});
    made.flush();
  }
  const auto placed = _buffers.emplace(name, std::move(made)).first;
  if (memory != Memory::upload)
    _fresh.push_back(&placed->second);
  const std::string_view where = memory_words[static_cast<std::size_t>(memory)];
  log(Level::info, Tag::mem, writer, std::format("{} of {} bytes {}", name, size, where));
}

// Params and buffer addresses are written once; the operator writes its values each
// frame. A kept block starts zeroed like a new one, so what a pass reads never depends
// on what its block held before (C01).
void Schedule::make_blocks() {
  for (Bound &bound : _bound) {
    if (!bound.errors.empty() || bound.block_size == 0) {
      bound.block.reset();
      continue;
    }
    if (!bound.block)
      bound.block.emplace(_wiring.pipelines, bound.block_size);
    std::ranges::fill(bound.block->bytes(), std::byte{});
    std::byte *const bytes = bound.block->bytes().data();
    for (const Field &field : bound.fields) {
      if (field.buffer()) {
        const VkDeviceAddress address =
            _buffers.at(buffer_name(bound.node->name, field.name)).address();
        std::memcpy(bytes + field.offset, &address, sizeof address);
      } else if (const Param *const param = bound.param(field.name)) {
        write_value(param->value, field.type, bytes + field.offset);
      }
    }
    bound.block->flush();
  }
  for (const Picture &picture : _images)
    point_readers(picture.name, picture.sampled.slot());
}

// A node that uploads once keeps what it filled through a rebuild, as long as it still
// fills it.
void Schedule::keep_images(Schedule &replaced) {
  for (const Bound &bound : _bound)
    for (const std::string &port : bound.textures) {
      const std::string name = std::format("{}.{}", bound.node->name, port);
      const auto kept = std::ranges::find(replaced._images, name, &Picture::name);
      if (kept == replaced._images.end())
        continue;
      _images.push_back(std::move(*kept));
      replaced._images.erase(kept);
    }
}

// Each block whose Texture samples the image holds its slot; any other Texture stays 0,
// unbound, until its image is filled.
void Schedule::point_readers(const std::string &name, std::uint32_t slot) {
  for (Bound &bound : _bound)
    for (const Field &field : bound.fields)
      if (bound.block && field.texture() &&
          image_name(bound.node->name, field.name) == name) {
        std::memcpy(bound.block->bytes().data() + field.offset, &slot, sizeof slot);
        bound.block->flush();
      }
}

// The copy runs before the frame's passes, from a buffer that lasts until the frame
// ends.
void Schedule::upload(const Bound &writer,
                      std::string_view port,
                      std::span<const std::byte> pixels,
                      VkExtent2D extent) {
  if (extent.width == 0 || extent.height == 0 ||
      pixels.size() != std::size_t{extent.width} * extent.height)
    throw std::runtime_error(
        std::format("the operator uploads {} pixels to {}, which is {} by {}",
                    pixels.size(),
                    port,
                    extent.width,
                    extent.height));
  const Image &image =
      image_for(writer, std::format("{}.{}", writer.node->name, port), extent);
  Buffer staged = _wiring.resources.buffer(
      pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::upload);
  std::ranges::copy(pixels, staged.bytes().begin());
  staged.flush();
  _copies.push_back({.from = staged.handle(), .to = image.handle(), .extent = extent});
  _staged.push_back(std::move(staged));
}

// The image of that size: a new one when the old has another, whose slot its readers
// take, and no copy into the old one runs.
const Image &
Schedule::image_for(const Bound &writer, const std::string &name, VkExtent2D extent) {
  const auto picture = std::ranges::find(_images, name, &Picture::name);
  if (picture != _images.end()) {
    const Image &image = picture->sampled.image();
    if (image.extent().width == extent.width && image.extent().height == extent.height)
      return image;
    std::erase_if(_copies, [&](const Copy &copy) { return copy.to == image.handle(); });
    _images.erase(picture);
  }
  const Picture &made = _images.emplace_back(
      name, Sampled(_wiring.pipelines, _wiring.resources.image(extent, pixel_format)));
  point_readers(name, made.sampled.slot());
  log(Level::info,
      Tag::mem,
      writer,
      std::format("{} of {} by {} pixels", name, extent.width, extent.height));
  return made.sampled.image();
}

void Schedule::make_passes() {
  for (const Bound &bound : _bound) {
    if (!bound.errors.empty() || !bound.pipeline)
      continue;
    const bool draw = is_draw(*bound.node);
    Pass pass{.bind_point =
                  draw ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE,
              .pipeline = bound.pipeline->handle(),
              .block = bound.block ? bound.block->set() : VK_NULL_HANDLE,
              .groups = draw ? 0
                             : bound.node->invocations /
                                   bound.shaders.front().workgroup_size()[0],
              .vertex_count = draw ? bound.node->vertex_count : 0,
              .instance_count = instance_number(*bound.node).value_or(0)};
    if (draw && !instance_number(*bound.node))
      pass.instances =
          &_used.at(buffer_name(bound.node->name, bound.node->instance_count));
    for (const Field &field : bound.fields) {
      if (!field.buffer())
        continue;
      const VkBuffer buffer =
          _buffers.at(buffer_name(bound.node->name, field.name)).handle();
      if (reads(field.access))
        pass.reads.push_back(buffer);
      if (writes(field.access))
        pass.writes.push_back(buffer);
    }
    _passes.push_back(std::move(pass));
  }
}

bool Schedule::ok() const {
  return std::ranges::all_of(_bound,
                             [](const Bound &bound) { return bound.errors.empty(); });
}

std::vector<VkBuffer> Schedule::take_clears() {
  std::vector<VkBuffer> clears;
  for (const Buffer *const buffer : _fresh)
    clears.push_back(buffer->handle());
  _fresh.clear();
  return clears;
}

// The frame before has ended, so the buffers its copies read go.
std::vector<Copy> Schedule::take_copies() {
  _in_flight = std::exchange(_staged, {});
  return std::exchange(_copies, {});
}

const std::vector<Pass> &Schedule::passes() const {
  return _passes;
}

// A node's own log level covers every line about it (V09).
void Schedule::log(Level level,
                   Tag tag,
                   const Bound &bound,
                   std::string_view text) const {
  _wiring.log.write(level, bound.log, tag, bound.node->name, text);
}

const Schedule::Held *Schedule::held(std::string_view name) const {
  const auto found = std::ranges::find(_objects, name, &Held::name);
  return found == _objects.end() ? nullptr : &*found;
}

Schedule::Bound *Schedule::find(std::string_view node) {
  const auto found = std::ranges::find_if(
      _bound, [&](const Bound &bound) { return bound.node->name == node; });
  return found == _bound.end() ? nullptr : &*found;
}

const Connection *Schedule::connection_of(std::string_view node,
                                          std::string_view port) const {
  const auto at = [&](const Endpoint &end) {
    return end.node == node && end.port == port;
  };
  for (const Connection &connection : _view.connections)
    if (at(connection.from) || std::ranges::any_of(connection.to, at))
      return &connection;
  return nullptr;
}

// An image goes by the port that fills it, so a connection made or removed keeps it.
std::string Schedule::image_name(std::string_view node, std::string_view port) const {
  if (const Connection *const connection = connection_of(node, port))
    return std::format("{}.{}", connection->from.node, connection->from.port);
  return std::format("{}.{}", node, port);
}

// A connected port's buffer is its connection's; an unconnected one is the node's own.
std::string Schedule::buffer_name(std::string_view node, std::string_view port) const {
  if (const Connection *const connection = connection_of(node, port))
    return connection->name;
  return std::format("{}.{}", node, port);
}

} // namespace VP
