// A view's GPU work, built from its graph. It runs past 800 lines (CPP12): the order of
// its nodes, the buffers and images their connections carry and the passes and
// barriers between them are worked out together, from one view, by one owner (A01).
#include "runtime/Schedule.h"

#include "baseclasses/Passes.h"
#include "runtime/Bound.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace VP {

namespace {

// Where a pass lands among the frame's passes while no frame gathers it, as the draws of
// a view that draws nowhere.
constexpr std::size_t no_pass = static_cast<std::size_t>(-1);

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

// An image a draw renders into, by its writer's node.port, or the window of a view this
// one hosts, by `<view>:`, as its readers' connection names it. The passes that draw it
// point at offscreen, which keeps its address while the schedule lives, so a new size
// reaches them with no gathering of passes.
struct Schedule::Rendered {
  std::string name;
  const PixelFormat *format = nullptr;
  VkRenderPass render_pass = VK_NULL_HANDLE; // which its draws' pipelines fit
  const Bound *writer = nullptr;             // null for a view's window
  std::optional<Drawn> drawn;                // none while there is no size
  Offscreen offscreen;
};

// An image a dispatch stores into, by its writer's node.port, as its readers' connection
// names it: made at the frame's size, zeroed, and made again whenever the size changes;
// none while there is no size. A rebuild keeps it, contents included, as it keeps a
// buffer, so what a dispatch stored once, as a frame a still holds, outlives the edit.
struct Schedule::Stored {
  std::string name;
  const Bound *writer = nullptr;
  std::optional<Sampled> image;
};

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
  // A view's window comes from its own schedule, whose passes run before this one's.
  for (const Connection &connection : view.connections)
    for (const Endpoint &to : connection.to)
      if (!connection.from.view() && to.node != connection.from.node) {
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

Schedule::Schedule(const Wiring &wiring,
                   std::string_view name,
                   const View &view,
                   Into into,
                   Schedule *replaced,
                   bool project)
    : _wiring(wiring), _name(name), _view(view), _joined(joined_of(view)),
      _window(into == Into::image
                  ? wiring.pipelines.render_pass(pixel_format(window_format)->format)
              : into == Into::window ? wiring.render_pass
                                     : VK_NULL_HANDLE),
      _into(into), _project(project) {
  const std::vector<const Node *> nodes = order(view);
  // Every node registers its commands again as it binds, so no command stays behind
  // with a node that went, and none blocks a node that takes its name.
  if (replaced)
    for (Bound &bound : replaced->_bound)
      replaced->drop_commands(bound);
  _bound.reserve(nodes.size());
  for (const Node *const node : nodes) {
    _bound.push_back(bind(*node, wiring.views / view.name / node->module, replaced));
    _places.emplace(node->name, _bound.size() - 1);
  }
  pair_draws(replaced);
  check_connections();
  check_images();
  list_ports();
  make_buffers(replaced);
  if (replaced)
    keep_images(*replaced);
  make_blocks();
  make_targets();
  if (replaced)
    keep_stored(*replaced);
  make_passes();
  cost_from(replaced);
  // Each error names the line that last added or changed the node, if a file holds it,
  // else the view's file, in its folder as the lines are. A node left out answers no
  // command either.
  const std::string file =
      (view.file.parent_path().filename() / view.file.filename()).generic_string();
  for (Bound &bound : _bound) {
    if (!bound.errors.empty())
      drop_commands(bound);
    // A node only waiting for the build says so quietly: the build says why if it fails.
    const bool waiting = bound.unbuilt || bound.waits == bound.errors.size();
    for (const std::string &error : bound.errors)
      _wiring.log.write(waiting ? Level::info : Level::error,
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

// A node whose SPIR-V the build did not rewrite keeps its reflection, and its pipeline
// while the pipeline still fits what it draws into.
void Schedule::load_shaders(Bound &bound,
                            const std::filesystem::path &folder,
                            Bound *old) {
  const std::vector<std::string> &shaders = bound.shaders_named = shaders_of(*bound.node);
  if (!check_counts(bound))
    return;
  for (const std::string &shader : shaders) {
    std::error_code missing;
    bound.spirv.push_back(spirv_of(folder, shader));
    bound.built.push_back(std::filesystem::last_write_time(bound.spirv.back(), missing));
    if (missing) {
      bound.errors.push_back(std::format("{} is not built: the build compiles it to {}",
                                         shader,
                                         bound.spirv.back().string()));
      return;
    }
  }
  const bool unchanged =
      old && old->loaded() && old->spirv == bound.spirv && old->built == bound.built;
  try {
    if (unchanged) {
      bound.shaders = std::move(old->shaders);
      bound.fields = std::move(old->fields);
      bound.block_size = old->block_size;
    } else {
      read_shaders(bound);
    }
    check_shaders(bound);
    // A vertex node's pipeline waits for its fragment node, which binds after it.
    if (!is_vertex(*bound.node) && !is_fragment(*bound.node))
      fit_pipeline(bound, old, unchanged);
  } catch (const std::exception &failure) {
    bound.shaders.clear();
    bound.fields.clear();
    bound.errors.emplace_back(failure.what());
    return;
  }
  check_stages(bound);
}

// A node's shaders share its one pass block, so they must agree on where each field is.
void Schedule::read_shaders(Bound &bound) const {
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
}

// unchanged: whether the build left the SPIR-V the old pipeline was made from alone.
void Schedule::fit_pipeline(Bound &bound, Bound *old, bool unchanged) {
  const VkRenderPass render_pass =
      is_draw(*bound.node) ? target_of(bound) : VK_NULL_HANDLE;
  if (unchanged && old->pipeline && old->render_pass == render_pass) {
    bound.pipeline = std::move(old->pipeline);
    bound.render_pass = render_pass;
    log(Level::debug, Tag::nod, bound, "keeps its pipeline");
  } else if (render_pass || !is_draw(*bound.node)) {
    make_pipeline(bound, render_pass);
  }
}

// A draw's one color goes into the view's window, unless a connection takes it to a
// Texture: then into an image, in the format the image word gives that port, of the node
// it is a port of, the fragment node of a draw split over two. Null for a format there
// is not, which check_images names, and for a window the view draws nowhere, which makes
// the draw no pipeline and no pass.
VkRenderPass Schedule::target_of(Bound &bound) const {
  Bound &colors = bound.fragment ? *bound.fragment : bound;
  const std::vector<Varying> &written = colors.stage(fragment_stage).outputs();
  if (written.size() != 1)
    throw std::runtime_error(std::format(
        "its .frag writes {} colors, but a draw writes one", written.size()));
  colors.output = written.front().name;
  if (!connection_of(colors.node->name, colors.output)) {
    if (!_window && _into != Into::none)
      throw std::runtime_error(std::format("it draws {} into the window, but none is "
                                           "open; a connection to a Texture draws it "
                                           "into an image",
                                           colors.output));
    return _window;
  }
  const std::vector<ImagePort> &words = colors.node->images;
  const auto word = std::ranges::find(words, colors.output, &ImagePort::port);
  colors.drawn = pixel_format(word == words.end() ? default_format : word->format);
  if (!colors.drawn)
    return VK_NULL_HANDLE;
  if (!_wiring.resources.samples(colors.drawn->format, Fill::draw))
    throw std::runtime_error(std::format(
        "{}: this GPU cannot draw into {} images", colors.output, colors.drawn->name));
  return _wiring.pipelines.render_pass(colors.drawn->format);
}

// A draw's pipeline fits the render pass it records in.
void Schedule::make_pipeline(Bound &bound, VkRenderPass render_pass) const {
  std::string from = joined(bound.shaders_named);
  if (!is_draw(*bound.node)) {
    bound.pipeline.emplace(_wiring.pipelines, bound.shaders.front());
  } else {
    const Bound &colors = bound.fragment ? *bound.fragment : bound;
    bound.pipeline.emplace(_wiring.pipelines,
                           bound.stage(vertex_stage),
                           colors.stage(fragment_stage),
                           render_pass);
    if (bound.fragment)
      from += std::format(" and {}'s {}",
                          colors.node->name,
                          colors.shaders_named.front());
  }
  bound.render_pass = render_pass;
  log(Level::info, Tag::nod, bound, "pipeline from " + from);
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
  for (Bound &bound : _bound)
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
  // C++ fills these for a connection to take to another node's shader, with the room it
  // asked for, as no shader of its node holds them.
  for (Bound &bound : _bound)
    for (const Bound::Written &written : bound.uploads)
      if (!bound.field(written.port) && written.count != 0)
        make_buffer(bound,
                    buffer_name(bound.node->name, written.port),
                    written.count,
                    written.stride,
                    Memory::upload,
                    replaced);
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

// The node that writes the buffer speaks for it, at that node's log level (V09). One the
// GPU cannot hold, as a count past what it has free makes, is the writer's error, before
// any memory is taken, so the node drops out and the run goes on (A02, A03).
void Schedule::make_buffer(Bound &writer,
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
  std::optional<Buffer> made;
  try {
    made.emplace(_wiring.resources.buffer(
        size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, memory));
  } catch (const std::runtime_error &failure) {
    writer.errors.push_back(
        std::format("{} of {} elements: {}", name, elements, failure.what()));
    _used.erase(name);
    return;
  }
  // The operator writes an upload buffer before the frame whose clears would zero it,
  // so the CPU zeroes it instead (C01).
  if (memory == Memory::upload) {
    std::ranges::fill(made->bytes(), std::byte{});
    made->flush();
  }
  const auto placed = _buffers.emplace(name, std::move(*made)).first;
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
        // Gone only where the GPU could not hold it, as its writer says.
        const auto made = _buffers.find(buffer_name(bound.node->name, field.name));
        if (made == _buffers.end()) {
          bound.errors.push_back(std::format(
              "{} has no buffer, which the GPU could not hold for its writer", field.name));
          break;
        }
        const VkDeviceAddress address = made->second.address();
        std::memcpy(bytes + field.offset, &address, sizeof address);
      } else if (const Param *const param = bound.param(field.name)) {
        write_value(param->value, field.type, bytes + field.offset);
      }
    }
    if (!bound.errors.empty())
      bound.block.reset();
    else
      bound.block->flush();
  }
  for (const Picture &picture : _images)
    point_readers(picture.name, picture.sampled.slot());
}

// A node that uploads once keeps what it filled through a rebuild, as long as it still
// fills it, in the same format.
void Schedule::keep_images(Schedule &replaced) {
  for (const Bound &bound : _bound)
    for (const Bound::Filled &filled : bound.textures) {
      const auto kept = std::ranges::find(replaced._images, filled.name, &Picture::name);
      if (kept == replaced._images.end() || !filled.format ||
          kept->sampled.image().format() != filled.format->format)
        continue;
      _images.push_back(std::move(*kept));
      replaced._images.erase(kept);
    }
}

// The images of the schedule replaced that its dispatches stored into, by name, as the
// dispatches that stay store into them still.
void Schedule::keep_stored(Schedule &replaced) {
  for (Stored &stored : _stored) {
    const auto kept = std::ranges::find(replaced._stored, stored.name, &Stored::name);
    if (kept == replaced._stored.end() || !kept->image)
      continue;
    stored.image = std::move(kept->image);
    kept->image.reset();
    point_readers(stored.name, stored.image->slot());
  }
}

// Each block whose Texture samples the image holds its slot, and so does the Image of
// the dispatch storing into it; any other stays 0, unbound, until its image is there.
void Schedule::point_readers(const std::string &name, std::uint32_t slot) {
  for (Bound &bound : _bound)
    for (const Field &field : bound.fields)
      if (bound.block && (field.texture() || field.image()) &&
          image_name(bound.node->name, field.name) == name) {
        std::memcpy(bound.block->bytes().data() + field.offset, &slot, sizeof slot);
        bound.block->flush();
      }
}

// The copy runs before the frame's passes, from a buffer that lasts until the frame
// ends.
void Schedule::upload(const Bound &writer,
                      std::uint32_t texture,
                      std::span<const std::byte> pixels,
                      VkExtent2D extent) {
  const Bound::Filled &filled = writer.textures.at(texture);
  const std::size_t count = pixels.size() / filled.format->bytes;
  if (extent.width == 0 || extent.height == 0 ||
      count != std::size_t{extent.width} * extent.height)
    throw std::runtime_error(
        std::format("the operator uploads {} pixels to {}, which is {} by {}",
                    count,
                    filled.port,
                    extent.width,
                    extent.height));
  const Image &image = image_for(writer, filled.name, extent, *filled.format);
  Buffer staged = _wiring.resources.buffer(
      pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::upload);
  std::ranges::copy(pixels, staged.bytes().begin());
  staged.flush();
  _copies.push_back({.from = staged.handle(), .to = image.handle(), .extent = extent});
  _staged.push_back(std::move(staged));
}

// The image of that size and format: a new one when the old has another, whose slot
// its readers take, and no copy into the old one runs.
const Image &Schedule::image_for(const Bound &writer,
                                 const std::string &name,
                                 VkExtent2D extent,
                                 const PixelFormat &format) {
  const auto picture = std::ranges::find(_images, name, &Picture::name);
  if (picture != _images.end()) {
    const Image &image = picture->sampled.image();
    if (image.extent().width == extent.width && image.extent().height == extent.height &&
        image.format() == format.format)
      return image;
  }
  // Made before the old one goes, so an image refused leaves the old one and the slot its
  // readers hold as they were, never a slot freed under them.
  Sampled sampled(_wiring.pipelines, _wiring.resources.image(extent, format.format));
  if (picture != _images.end()) {
    const VkImage old = picture->sampled.image().handle();
    std::erase_if(_copies, [&](const Copy &copy) { return copy.to == old; });
    _images.erase(picture);
  }
  const Picture &made = _images.emplace_back(name, std::move(sampled));
  point_readers(name, made.sampled.slot());
  log(Level::info,
      Tag::mem,
      writer,
      std::format(
          "{}: {} by {} pixels of {}", name, extent.width, extent.height, format.name));
  return made.sampled.image();
}

// An image for each draw whose color a connection takes to a Texture, for each Image a
// dispatch stores into, and for each view this one hosts whose window a connection
// takes, made as the first frame gives it a size. A view's window is there while its
// connection is, so the hosted view's draws, whose pipelines fit it, always have it to
// draw into.
void Schedule::make_targets() {
  for (const Bound &bound : _bound) {
    if (!bound.errors.empty() || !bound.pipeline)
      continue;
    const Bound &colors = bound.fragment ? *bound.fragment : bound;
    if (colors.drawn)
      _rendered.push_back(std::make_unique<Rendered>(
          Rendered{.name = std::format("{}.{}", colors.node->name, colors.output),
                   .format = colors.drawn,
                   .render_pass = bound.render_pass,
                   .writer = &bound}));
    for (const Field &field : bound.fields)
      if (field.image())
        _stored.push_back({.name = std::format("{}.{}", bound.node->name, field.name),
                           .writer = &bound});
  }
  const PixelFormat *const window = pixel_format(window_format);
  for (const Connection &connection : _view.connections)
    if (connection.from.view())
      _rendered.push_back(std::make_unique<Rendered>(
          Rendered{.name = connection.from.text(),
                   .format = window,
                   .render_pass = _wiring.pipelines.render_pass(window->format)}));
}

// To the frame's size, before any pass runs: an image of another size goes, and its
// readers sample nothing until the new one is drawn. With no size there is none, so its
// draws record nothing: no room, no work.
void Schedule::size_targets(VkExtent2D size) {
  for (const std::unique_ptr<Rendered> &rendered : _rendered) {
    const VkExtent2D now = rendered->offscreen.extent;
    if (now.width == size.width && now.height == size.height)
      continue;
    rendered->offscreen = {};
    rendered->drawn.reset();
    const PixelFormat &format = *rendered->format;
    if (size.width != 0 && size.height != 0) {
      const Drawn &drawn = rendered->drawn.emplace(
          _wiring.pipelines, _wiring.resources.image(size, format.format, Fill::draw));
      rendered->offscreen = {.render_pass = rendered->render_pass,
                             .framebuffer = drawn.framebuffer(),
                             .extent = size};
      const std::string pixels =
          std::format("{} by {} pixels of {}", size.width, size.height, format.name);
      if (rendered->writer)
        log(Level::info,
            Tag::mem,
            *rendered->writer,
            std::format("{}: {}, which it draws", rendered->name, pixels));
      else
        _wiring.log.write(
            Level::info,
            Tag::mem,
            std::format("{} draws its window into {}", rendered->name, pixels));
    }
    const auto instead = _instead.find(rendered->name);
    point_readers(rendered->name,
                  instead != _instead.end() ? instead->second
                  : rendered->drawn         ? rendered->drawn->sampled().slot()
                                            : 0);
  }
  for (Stored &stored : _stored)
    size_stored(stored, size);
}

// A new image is zeroed before the passes of the frame that first runs it, so what a
// dispatch leaves unstored samples as nothing at all, the same on every run (C01).
void Schedule::size_stored(Stored &stored, VkExtent2D size) {
  if (stored.image && stored.image->image().extent().width == size.width &&
      stored.image->image().extent().height == size.height)
    return;
  if (stored.image) {
    const VkImage old = stored.image->image().handle();
    std::erase_if(_copies, [&](const Copy &copy) { return copy.to == old; });
    stored.image.reset();
  }
  const PixelFormat &format = *pixel_format(stored_format);
  if (size.width != 0 && size.height != 0) {
    const Sampled &made = stored.image.emplace(
        _wiring.pipelines, _wiring.resources.image(size, format.format, Fill::store));
    _copies.push_back({.to = made.image().handle(), .extent = size});
    log(Level::info,
        Tag::mem,
        *stored.writer,
        std::format("{}: {} by {} pixels of {}, which it stores into",
                    stored.name,
                    size.width,
                    size.height,
                    format.name));
  }
  point_readers(stored.name, stored.image ? stored.image->slot() : 0);
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
              .instance_count = instance_number(*bound.node).value_or(0),
              .stores = std::ranges::any_of(bound.fields, &Field::image)};
    if (draw && !instance_number(*bound.node))
      pass.instances =
          &_used.at(buffer_name(bound.node->name, bound.node->instance_count));
    for (const std::unique_ptr<Rendered> &rendered : _rendered)
      if (rendered->writer == &bound)
        pass.offscreen = &rendered->offscreen;
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
    _passed.push_back(static_cast<std::size_t>(&bound - _bound.data()));
  }
}

// A node keeps what it cost through a rebuild, as an edit changes nothing it does, so
// its smoothed times go on from where they were.
void Schedule::cost_from(const Schedule *replaced) {
  _costs.reserve(_bound.size());
  for (const Bound &bound : _bound)
    _costs.push_back({.node = bound.node->name});
  for (const std::size_t at : _passed)
    _costs[at].runs = true;
  if (!replaced)
    return;
  for (Cost &cost : _costs) {
    const auto old = std::ranges::find(replaced->_costs, cost.node, &Cost::node);
    if (old == replaced->_costs.end())
      continue;
    cost.cpu = old->cpu;
    cost.cpu_last = old->cpu_last;
    cost.cooks = old->cooks;
    cost.gpu = old->gpu;
    cost.gpu_last = old->gpu_last;
    cost.timed = old->timed && cost.runs;
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

// Between frames or during one, before the passes run, so no pass samples the image.
void Schedule::clear_image(std::string_view node, std::string_view port) {
  const Connection *const connection = connection_of(node, port);
  const Endpoint writer =
      connection ? connection->from : Endpoint{std::string(node), std::string(port)};
  const Bound *const filler = find(writer.node);
  if (!filler || !filler->fills(writer.port))
    throw std::runtime_error(
        std::format("{}.{} is no image a node's C++ fills", node, port));
  const std::string name = std::format("{}.{}", writer.node, writer.port);
  const auto picture = std::ranges::find(_images, name, &Picture::name);
  if (picture == _images.end())
    return;
  const VkImage image = picture->sampled.image().handle();
  std::erase_if(_copies, [&](const Copy &copy) { return copy.to == image; });
  _images.erase(picture);
  point_readers(name, 0);
}

// The frame before has ended, so the buffers its copies read go.
std::vector<Copy> Schedule::take_copies() {
  _in_flight = std::exchange(_staged, {});
  return std::exchange(_copies, {});
}

void Schedule::rest(bool rests) {
  _resting = rests;
}

bool Schedule::resting() const {
  return _resting;
}

void Schedule::work(std::vector<Pass> &passes) {
  _landed.assign(_passes.size(), no_pass);
  for (std::size_t at = 0; at < _passes.size() && !_resting; ++at)
    if ((_pulled.empty() || _pulled[_passed[at]]) &&
        (_passes[at].bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS ||
         _passes[at].offscreen)) {
      _landed[at] = passes.size();
      passes.push_back(_passes[at]);
    }
}

bool Schedule::draws(std::vector<Pass> &passes, const Offscreen *into) {
  if (_landed.size() != _passes.size())
    _landed.assign(_passes.size(), no_pass);
  const std::size_t before = passes.size();
  for (std::size_t at = 0; at < _passes.size() && !_resting; ++at)
    if ((_pulled.empty() || _pulled[_passed[at]]) &&
        _passes[at].bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS &&
        !_passes[at].offscreen) {
      _landed[at] = passes.size();
      passes.push_back(_passes[at]);
      passes.back().offscreen = into;
    }
  return passes.size() != before;
}

void Schedule::clears(std::vector<Pass> &passes, const Offscreen *into) {
  passes.push_back({.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
                    .instance_count = 0,
                    .offscreen = into});
}

std::span<const Cost> Schedule::costs() const {
  return _costs;
}

std::span<const NodePort> Schedule::ports() const {
  return _ports;
}

void Schedule::time(std::span<const float> took) {
  for (std::size_t at = 0; at < _landed.size(); ++at)
    if (_landed[at] < took.size() && !std::isnan(took[_landed[at]])) {
      Cost &cost = _costs[_passed[at]];
      smooth(cost.gpu, cost.gpu_last, cost.timed, took[_landed[at]]);
    }
}

const Offscreen *Schedule::window_of(std::string_view view) const {
  const std::string name = Endpoint{std::string(view), {}}.text();
  for (const std::unique_ptr<Rendered> &rendered : _rendered)
    if (!rendered->writer && rendered->name == name)
      return &rendered->offscreen;
  return nullptr;
}

// The nodes the image's writer needs, and the nodes they need, as far back as the
// connections go: a draw split in two counts both its nodes, and a node already counted
// stops the walk, as a feedback loop would go round.
bool Schedule::pull(std::string_view image) {
  if (image == _pulled_for)
    return false;
  _pulled_for.assign(image);
  _pulled.clear();
  if (image.empty())
    return true;
  _pulled.assign(_bound.size(), false);
  std::vector<std::string_view> wanted{image.substr(0, image.rfind('.'))};
  while (!wanted.empty()) {
    const std::string_view node = wanted.back();
    wanted.pop_back();
    for (std::size_t at = 0; at < _bound.size(); ++at) {
      const Bound &bound = _bound[at];
      const bool colors = bound.fragment && bound.fragment->node->name == node;
      if (_pulled[at] || (bound.node->name != node && !colors))
        continue;
      _pulled[at] = true;
      if (bound.fragment)
        wanted.push_back(bound.fragment->node->name);
      for (const Connection &connection : _view.connections)
        if (!connection.from.view() &&
            std::ranges::any_of(connection.to, [&](const Endpoint &to) {
              return to.node == bound.node->name;
            }))
          wanted.push_back(connection.from.node);
    }
  }
  return true;
}

std::size_t Schedule::running() const {
  return _pulled.empty() ? _bound.size()
                         : static_cast<std::size_t>(std::ranges::count(_pulled, true));
}

std::optional<std::uint32_t> Schedule::image_slot(std::string_view image) const {
  for (const std::unique_ptr<Rendered> &rendered : _rendered)
    if (rendered->writer && rendered->name == image)
      return rendered->drawn ? rendered->drawn->sampled().slot() : 0;
  for (const Stored &stored : _stored)
    if (stored.name == image)
      return stored.image ? stored.image->slot() : 0;
  for (const Picture &picture : _images)
    if (picture.name == image)
      return picture.sampled.slot();
  for (const Bound &bound : _bound)
    if (std::ranges::any_of(bound.textures, [&](const Bound::Filled &filled) {
          return filled.name == image;
        }))
      return 0;
  return std::nullopt;
}

std::vector<std::string> Schedule::images() const {
  std::vector<std::string> names;
  for (const std::unique_ptr<Rendered> &rendered : _rendered)
    if (rendered->writer)
      names.push_back(rendered->name);
  for (const Stored &stored : _stored)
    names.push_back(stored.name);
  for (const Bound &bound : _bound)
    for (const Bound::Filled &filled : bound.textures)
      names.push_back(filled.name);
  return names;
}

void Schedule::show_instead(const std::string &window,
                            std::optional<std::uint32_t> slot) {
  if (slot)
    _instead.insert_or_assign(window, *slot);
  else
    _instead.erase(window);
  for (const std::unique_ptr<Rendered> &rendered : _rendered)
    if (!rendered->writer && rendered->name == window)
      point_readers(window,
                    slot             ? *slot
                    : rendered->drawn ? rendered->drawn->sampled().slot()
                                      : 0);
}

// A node's own log level covers every line about it (V09).
void Schedule::log(Level level,
                   Tag tag,
                   const Bound &bound,
                   std::string_view text) const {
  _wiring.log.write(level, bound.log, tag, _name, bound.node->name, text);
}

const Held *Schedule::held(std::string_view name) const {
  const auto found = std::ranges::find(_objects, name, &Held::name);
  return found == _objects.end() ? nullptr : &*found;
}

// In the view's order, as a scan of it finds them.
Schedule::Joined Schedule::joined_of(const View &view) {
  Joined joined;
  for (const Connection &connection : view.connections) {
    joined.try_emplace({connection.from.node, connection.from.port}, &connection);
    for (const Endpoint &to : connection.to)
      joined.try_emplace({to.node, to.port}, &connection);
  }
  return joined;
}

// A node binding finds those bound before it.
Bound *Schedule::find(std::string_view node) {
  const auto found = _places.find(node);
  return found == _places.end() ? nullptr : &_bound[found->second];
}

const Connection *Schedule::connection_of(std::string_view node,
                                          std::string_view port) const {
  const auto found = _joined.find({node, port});
  return found == _joined.end() ? nullptr : found->second;
}

// An image goes by the port that fills it, so a connection made or removed keeps it, or
// by the view whose window it is.
std::string Schedule::image_name(std::string_view node, std::string_view port) const {
  if (const Connection *const connection = connection_of(node, port))
    return connection->from.text();
  return std::format("{}.{}", node, port);
}

// A connected port's buffer is its connection's; an unconnected one is the node's own.
std::string Schedule::buffer_name(std::string_view node, std::string_view port) const {
  if (const Connection *const connection = connection_of(node, port))
    return connection->name;
  return std::format("{}.{}", node, port);
}

} // namespace VP
