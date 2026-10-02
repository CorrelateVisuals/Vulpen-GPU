#include "runtime/Schedule.h"

#include "baseclasses/Passes.h"
#include "baseclasses/Pipelines.h"
#include "runtime/Operator.h"
#include "runtime/Recipes.h"
#include "runtime/View.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <set>
#include <stdexcept>

namespace VP {

namespace {

template <class T> bool parse(std::string_view text, std::byte *out) {
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return false;
  if (out)
    std::memcpy(out, &value, sizeof value);
  return true;
}

// A param's text as the bytes of the field's GLSL type; false when it does not parse.
bool write_value(std::string_view text, std::string_view type, std::byte *out) {
  if (type == glsl_type<float>)
    return parse<float>(text, out);
  if (type == glsl_type<std::int32_t>)
    return parse<std::int32_t>(text, out);
  if (type == glsl_type<std::uint32_t>)
    return parse<std::uint32_t>(text, out);
  return false;
}

bool reads(Access access) {
  return access != Access::write;
}

bool writes(Access access) {
  return access != Access::read;
}

// The build compiles each shader as the stage its extension names, as glslang does, so
// the loader reads the stage from the same place.
constexpr std::string_view compute_stage = ".comp";
constexpr std::string_view vertex_stage = ".vert";
constexpr std::string_view fragment_stage = ".frag";

std::ptrdiff_t count_stage(const Node &node, std::string_view stage) {
  return std::ranges::count_if(
      node.shaders, [&](const std::string &shader) { return shader.ends_with(stage); });
}

bool is_draw(const Node &node) {
  return count_stage(node, vertex_stage) != 0;
}

// Views own their copies of recipes (V03), so the build names a recipe "view/recipe".
std::string recipe_of(const View &view, const Node &node) {
  return std::format("{}/{}", view.name, node.recipe);
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

struct Schedule::Bound {
  const Node *node = nullptr;
  Level log = Level::warn;
  std::vector<std::filesystem::path> spirv; // by the node's shaders
  std::vector<std::filesystem::file_time_type> built;
  std::vector<Shader> shaders; // all of the node's, or none when one fails to load
  std::vector<Field> fields;   // the pass block its shaders share
  std::uint32_t block_size = 0;
  std::optional<Pipeline> pipeline;
  std::optional<PassBlock> block;
  std::unique_ptr<Operator> op;
  std::set<std::string, std::less<>> set_by_operator;
  std::set<std::string, std::less<>> read_by_operator;
  std::vector<std::string> readbacks;           // ports, by Readback<T>::index
  std::vector<const Buffer *> readback_buffers; // the same, once the buffers exist
  std::vector<std::string> uploads;             // ports, by Upload<T>::index
  std::vector<const Buffer *> upload_buffers;
  std::vector<std::string> errors;

  bool loaded() const {
    return !shaders.empty();
  }
  const Field *field(std::string_view name) const {
    const auto found = std::ranges::find(fields, name, &Field::name);
    return found == fields.end() ? nullptr : &*found;
  }
  bool uploads_to(std::string_view port) const {
    return std::ranges::find(uploads, port) != uploads.end();
  }
  const Param *param(std::string_view key) const {
    const auto found = std::ranges::find(node->params, key, &Param::key);
    return found == node->params.end() ? nullptr : &*found;
  }
};

// Resolves an operator's names against its node while it binds. A name that does not
// fit becomes one of the node's errors, and the operator gets a harmless handle.
class Schedule::Binder final : public Bind {
public:
  explicit Binder(Bound &bound) : _bound(bound) {}

private:
  std::uint32_t value_offset(std::string_view name, std::string_view type) override {
    const Field *const field = _bound.field(name);
    if (!field || field->buffer()) {
      _bound.errors.push_back(std::format(
          "the operator sets {}, which the shader's pass block does not hold as a value",
          name));
      return 0;
    }
    if (field->type != type) {
      _bound.errors.push_back(
          std::format("the operator sets {} as a {}, but the shader declares a {}",
                      name,
                      type,
                      field->type));
      return 0;
    }
    _bound.set_by_operator.emplace(name);
    return field->offset;
  }

  std::uint32_t readback_index(std::string_view name,
                               std::size_t element_size,
                               std::string_view type) override {
    const Field *const field = _bound.field(name);
    if (!field || !field->buffer())
      _bound.errors.push_back(std::format(
          "the operator reads back {}, which is not a buffer of its shader", name));
    else
      check_element(*field, "reads", element_size, type);
    _bound.readbacks.emplace_back(name);
    return static_cast<std::uint32_t>(_bound.readbacks.size() - 1);
  }

  // Only the operator writes the buffer, so its shaders declare it readonly.
  std::uint32_t upload_index(std::string_view name,
                             std::size_t element_size,
                             std::string_view type) override {
    const Field *const field = _bound.field(name);
    if (!field || !field->buffer())
      _bound.errors.push_back(std::format(
          "the operator writes {}, which is not a buffer of its shader", name));
    else if (field->access != Access::read)
      _bound.errors.push_back(std::format(
          "the operator writes {}, which its shader writes too; declare it readonly",
          name));
    else
      check_element(*field, "writes", element_size, type);
    _bound.uploads.emplace_back(name);
    return static_cast<std::uint32_t>(_bound.uploads.size() - 1);
  }

  void check_element(const Field &field,
                     std::string_view verb,
                     std::size_t element_size,
                     std::string_view type) {
    if (field.stride != element_size || (!type.empty() && type != field.type))
      _bound.errors.push_back(
          std::format("the operator {} {} as {}-byte {}, but the shader holds {}-byte {}",
                      verb,
                      field.name,
                      element_size,
                      type.empty() ? "elements" : type,
                      field.stride,
                      field.type));
  }

  std::string_view param_text(std::string_view name) override {
    _bound.read_by_operator.emplace(name);
    if (const Param *const param = _bound.param(name))
      return param->value;
    _bound.errors.push_back(
        std::format("the operator reads param {}, which the node does not set", name));
    return {};
  }

  void param_invalid(std::string_view name, std::string_view type) override {
    if (const Param *const param = _bound.param(name))
      _bound.errors.push_back(
          std::format("param {} = {} is not a {}", name, param->value, type));
  }

  Bound &_bound;
};

// What an operator reaches during a frame: its node's block and read-backs, nothing else.
class Schedule::Cooker final : public Cook {
public:
  Cooker(const Schedule &schedule, Bound &bound, std::uint64_t frame)
      : _schedule(schedule), _bound(bound), _frame(frame) {}

private:
  std::uint64_t index() const override {
    return _frame;
  }
  void log(Level level, std::string_view text) const override {
    _schedule.log(level, Tag::out, _bound, text);
  }
  std::span<std::byte> block() override {
    return _bound.block->bytes();
  }
  std::span<const std::byte> readback_bytes(std::uint32_t index) const override {
    const Buffer *const buffer = _bound.readback_buffers.at(index);
    if (std::ranges::find(_schedule._fresh, buffer) != _schedule._fresh.end())
      return {};
    return buffer->bytes();
  }
  std::span<std::byte> upload_bytes(std::uint32_t index) override {
    return _bound.upload_buffers.at(index)->bytes();
  }

  const Schedule &_schedule;
  Bound &_bound;
  const std::uint64_t _frame;
};

// Of the nodes whose writers are all placed, the first in the manifest goes next, so
// nodes the connections leave unordered, such as two draws, run as the manifest lists
// them. Every edit reorders the view, so the connections are indexed once, not searched
// for each node placed.
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
  _bound.reserve(nodes.size());
  for (const Node *const node : nodes)
    _bound.push_back(
        bind(*node, wiring.views / view.name / "recipes" / node->recipe, replaced));
  check_connections();
  make_buffers(replaced);
  make_blocks();
  make_passes();
  // Each error names the line that last added or changed the node, if a file holds it.
  for (const Bound &bound : _bound)
    for (const std::string &error : bound.errors)
      _wiring.log.write(Level::error,
                        Tag::nod,
                        std::format("{} node {}: {}",
                                    bound.node->where.empty()
                                        ? view.file.filename().string()
                                        : bound.node->where,
                                    bound.node->name,
                                    error));
}

Schedule::~Schedule() = default;

Schedule::Bound Schedule::bind(const Node &node,
                               const std::filesystem::path &folder,
                               Schedule *replaced) {
  Bound bound{.node = &node, .log = _wiring.log.level()};
  if (!node.log.empty()) {
    if (const std::optional<Level> level = level_named(node.log))
      bound.log = *level;
    else
      bound.errors.push_back(
          std::format("log = {} is not error, warn, info or debug", node.log));
  }
  Bound *const old = replaced ? replaced->find(node.name) : nullptr;
  load_shaders(bound, folder, old);
  // So an edit makes blocks only for the nodes it changed.
  if (old && old->block && old->block_size == bound.block_size)
    bound.block = std::move(old->block);
  if (old && old->op && old->node->recipe == node.recipe &&
      old->node->operator_name == node.operator_name) {
    bound.op = std::move(old->op);
    log(Level::debug, Tag::nod, bound, "keeps its operator " + node.operator_name);
  } else if (!node.operator_name.empty()) {
    try {
      bound.op =
          _wiring.recipes.make(recipe_of(_view, node), folder, node.operator_name);
      log(Level::info, Tag::nod, bound, "new operator " + node.operator_name);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
    }
  }
  if (bound.op) {
    Binder binder(bound);
    try {
      bound.op->bind(binder);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
    }
  }
  check_fields(bound);
  return bound;
}

// A node whose SPIR-V the build did not rewrite keeps its reflection and pipeline.
void Schedule::load_shaders(Bound &bound,
                            const std::filesystem::path &folder,
                            Bound *old) {
  const Node &node = *bound.node;
  if (node.shaders.empty())
    return;
  const bool dispatch = node.shaders.size() == 1 && count_stage(node, compute_stage) == 1;
  const bool draw = node.shaders.size() == 2 && count_stage(node, vertex_stage) == 1 &&
                    count_stage(node, fragment_stage) == 1;
  if (!dispatch && !draw) {
    bound.errors.push_back("a node runs one .comp shader, or one .vert and one .frag");
    return;
  }
  for (const std::string &shader : node.shaders) {
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
    throw std::runtime_error("it draws, but vulpen opened no window for this view; "
                             "restart vulpen to open one");
  } else {
    const std::size_t vertex = node.shaders.front().ends_with(vertex_stage) ? 0 : 1;
    bound.pipeline.emplace(_wiring.pipelines,
                           bound.shaders[vertex],
                           bound.shaders[1 - vertex],
                           _wiring.render_pass);
  }
  std::string names;
  for (const std::string &shader : node.shaders)
    names += (names.empty() ? "" : " and ") + shader;
  log(Level::info, Tag::nod, bound, "pipeline from " + names);
  if (!bound.fields.empty())
    log(Level::debug, Tag::nod, bound, pass_block(bound.fields, bound.block_size));
}

// A draw's shaders only read buffers: storing from them needs features beyond the
// Vulkan floor (RVK01).
void Schedule::check_stages(Bound &bound) const {
  const Node &node = *bound.node;
  if (is_draw(node)) {
    for (const Field &field : bound.fields)
      if (field.buffer() && field.access != Access::read)
        bound.errors.push_back(std::format(
            "{}: a draw's shaders only read buffers; declare it readonly", field.name));
    return;
  }
  const std::array<std::uint32_t, 3> &size = bound.shaders.front().workgroup_size();
  if (size[1] != 1 || size[2] != 1)
    bound.errors.push_back("only one-dimensional workgroups run yet: local_size_y and "
                           "local_size_z must be 1");
  else if (node.invocations % size[0] != 0)
    bound.errors.push_back(
        std::format("invocations = {} is not a multiple of local_size_x = {}",
                    node.invocations,
                    size[0]));
}

// Every param is read by someone, and every value in the pass block is set by someone.
// Without its operator, a node cannot say what the operator would have read or set.
void Schedule::check_fields(Bound &bound) const {
  if (!bound.node->operator_name.empty() && !bound.op)
    return;
  for (const Param &param : bound.node->params) {
    const Field *const field = bound.field(param.key);
    if (field && !field->buffer()) {
      if (bound.set_by_operator.contains(param.key))
        bound.errors.push_back(
            std::format("{} is set both by a param and by the operator", param.key));
      else if (!write_value(param.value, field->type, nullptr))
        bound.errors.push_back(std::format(
            "param {} = {} is not a {}", param.key, param.value, field->type));
    } else if (!bound.read_by_operator.contains(param.key)) {
      bound.errors.push_back(std::format("param {}: nothing reads it", param.key));
    }
  }
  for (const Field &field : bound.fields)
    if (!field.buffer() && !bound.param(field.name) &&
        !bound.set_by_operator.contains(field.name))
      bound.errors.push_back(std::format("{} in the pass block: nothing sets it; give "
                                         "the node a param {} or set it from "
                                         "the operator",
                                         field.name,
                                         field.name));
}

void Schedule::check_connections() {
  for (const Connection &connection : _view.connections) {
    Bound &writer = *find(connection.from.node);
    const Field *const out = writer.field(connection.from.port);
    const bool written = out && out->buffer() &&
                         (writes(out->access) || writer.uploads_to(connection.from.port));
    if (writer.loaded() && !written)
      writer.errors.push_back(
          std::format("connection {}: {} is not a buffer its shader or operator writes",
                      connection.name,
                      connection.from.port));
    for (const Endpoint &to : connection.to) {
      Bound &reader = *find(to.node);
      const Field *const in = reader.field(to.port);
      if (!reader.loaded())
        continue;
      if (!written)
        reader.errors.push_back(
            std::format("connection {}: nothing writes it", connection.name));
      else if (!in || !in->buffer() || !reads(in->access))
        reader.errors.push_back(
            std::format("connection {}: {} is not a buffer its shader reads",
                        connection.name,
                        to.port));
      else if (in->stride != out->stride || in->type != out->type)
        reader.errors.push_back(
            std::format("connection {}: {} writes {}-byte {}, {} reads "
                        "{}-byte {}",
                        connection.name,
                        connection.from.node,
                        out->stride,
                        out->type,
                        to.port,
                        in->stride,
                        in->type));
    }
  }
  for (Bound &bound : _bound)
    for (const Field &field : bound.fields) {
      if (!field.buffer() || field.access != Access::read)
        continue;
      const Connection *const connection = connection_of(bound.node->name, field.name);
      const bool uploaded = bound.uploads_to(field.name);
      if (!connection && !uploaded)
        bound.errors.push_back(
            std::format("{} reads nothing: connect it to a buffer another node writes, "
                        "or write it from the operator",
                        field.name));
      else if (connection && uploaded && connection->from.node != bound.node->name)
        bound.errors.push_back(
            std::format("{} is written both by the operator and through connection {}",
                        field.name,
                        connection->name));
    }
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
      const bool uploaded = bound.uploads_to(field.name);
      if (!field.buffer() || (!writes(field.access) && !uploaded))
        continue;
      const std::string name = buffer_name(bound.node->name, field.name);
      const Memory memory = uploaded                   ? Memory::upload
                            : read_back.contains(name) ? Memory::readback
                                                       : Memory::device;
      make_buffer(bound,
                  name,
                  VkDeviceSize{bound.node->invocations} * field.stride,
                  memory,
                  replaced);
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
    bound.upload_buffers = buffers_of(bound, bound.uploads);
  }
}

// The node that writes the buffer speaks for it, at that node's log level (V09).
void Schedule::make_buffer(const Bound &writer,
                           const std::string &name,
                           VkDeviceSize size,
                           Memory memory,
                           Schedule *replaced) {
  if (_buffers.contains(name))
    return;
  if (replaced) {
    auto kept = replaced->_buffers.extract(name);
    if (kept && kept.mapped().size() == size && kept.mapped().memory() == memory) {
      // Edits rebuild with no frame between, so one no frame has zeroed yet stays fresh.
      if (std::ranges::find(replaced->_fresh, &kept.mapped()) != replaced->_fresh.end())
        _fresh.push_back(&kept.mapped());
      _buffers.insert(std::move(kept));
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
}

void Schedule::make_passes() {
  for (const Bound &bound : _bound) {
    if (!bound.errors.empty() || !bound.pipeline)
      continue;
    const bool draw = is_draw(*bound.node);
    const std::uint32_t invocations = bound.node->invocations;
    Pass pass{.bind_point = draw ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                 : VK_PIPELINE_BIND_POINT_COMPUTE,
              .pipeline = bound.pipeline->handle(),
              .block = bound.block ? bound.block->set() : VK_NULL_HANDLE,
              .groups =
                  draw ? 0 : invocations / bound.shaders.front().workgroup_size()[0],
              .vertex_count = draw ? invocations : 0};
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

void Schedule::drop_operators(const std::vector<std::string> &recipes) {
  for (Bound &bound : _bound)
    if (std::ranges::find(recipes, recipe_of(_view, *bound.node)) != recipes.end())
      bound.op.reset();
}

void Schedule::cook(std::uint64_t frame) {
  for (const auto &entry : _buffers)
    if (entry.second.memory() == Memory::readback)
      entry.second.invalidate();
  for (Bound &bound : _bound) {
    if (!bound.errors.empty() || !bound.op)
      continue;
    Cooker cooker(*this, bound, frame);
    try {
      bound.op->cook(cooker);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
      _wiring.log.write(Level::error,
                        Tag::nod,
                        std::format("node {}: {}; its operator stops",
                                    bound.node->name,
                                    failure.what()));
    }
    if (bound.block)
      bound.block->flush();
    for (const Buffer *const buffer : bound.upload_buffers)
      buffer->flush();
  }
}

std::vector<VkBuffer> Schedule::take_clears() {
  std::vector<VkBuffer> clears;
  for (const Buffer *const buffer : _fresh)
    clears.push_back(buffer->handle());
  _fresh.clear();
  return clears;
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

// A connected port's buffer is its connection's; an unconnected one is the node's own.
std::string Schedule::buffer_name(std::string_view node, std::string_view port) const {
  if (const Connection *const connection = connection_of(node, port))
    return connection->name;
  return std::format("{}.{}", node, port);
}

} // namespace VP
