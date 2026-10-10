#include "runtime/Bound.h"

#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Modules.h"
#include "runtime/Ports.h"

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>

namespace VP {

namespace {

// Views own their copies (V03), so the build names a node's module "view/folder".
std::string module_of(const View &view, const Node &node) {
  return std::format("{}/{}", view.name, node.module.generic_string());
}

} // namespace

// Resolves an operator's names against its node while it binds. A name that does not
// fit becomes one of the node's errors, and the operator gets a harmless handle.
class Schedule::Binder final : public Bind {
public:
  Binder(Schedule &schedule, Bound &bound, Schedule *replaced)
      : _schedule(schedule), _bound(bound), _replaced(replaced),
        _commands(schedule._wiring.commands), _ports(schedule._wiring.ports) {}

  Command command(std::string_view usage, std::string_view help) override {
    try {
      return _bound.commands.emplace_back(_commands.add(usage, help, *_bound.op));
    } catch (const std::runtime_error &failure) {
      _bound.errors.emplace_back(failure.what());
      return {};
    }
  }
  File file(std::string_view path) override {
    const std::filesystem::path named(path);
    try {
      return _bound.files.emplace_back(
          _ports.open(named.is_relative() ? _bound.node->folder / named : named));
    } catch (const std::runtime_error &failure) {
      _bound.errors.emplace_back(failure.what());
      return {};
    }
  }
  std::string folder() const override {
    return _bound.node->folder.string();
  }
  std::string name() const override {
    return _bound.node->name;
  }
  const Engine &engine() const override {
    return _schedule._wiring.engine;
  }

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

  std::uint32_t readback_index(std::string_view name, const Element &element) override {
    const Field *const field = _bound.field(name);
    if (!field || !field->buffer())
      _bound.errors.push_back(std::format(
          "the operator reads back {}, which is not a buffer of its shader", name));
    else
      check_element(*field, "reads", element);
    _bound.readbacks.emplace_back(name);
    return static_cast<std::uint32_t>(_bound.readbacks.size() - 1);
  }

  // A buffer of its shaders, which they declare readonly, as only the operator writes it;
  // or, given room, a port no shader of the node holds, which a connection takes to
  // another node's shader, whose struct check_connections holds it to.
  std::uint32_t upload_index(std::string_view name,
                             const Element &element,
                             std::uint32_t count) override {
    const Field *const field = _bound.field(name);
    Field written = element_field(name, element);
    if (!field && count == 0)
      _bound.errors.push_back(
          std::format("the operator writes {}, which no shader of its node holds; give "
                      "it room, node.upload<T>(\"{}\", count), for a connection to take "
                      "to another node's shader",
                      name,
                      name));
    else if (field && !field->buffer())
      _bound.errors.push_back(std::format(
          "the operator writes {}, which is not a buffer of its shader", name));
    else if (field && field->access != Access::read)
      _bound.errors.push_back(std::format(
          "the operator writes {}, which its shader writes too; declare it readonly",
          name));
    else if (field)
      check_element(*field, "writes", element);
    _bound.uploads.push_back({.port = std::string(name),
                              .count = count,
                              .stride = static_cast<std::uint32_t>(element.size),
                              .element = std::move(written)});
    return static_cast<std::uint32_t>(_bound.uploads.size() - 1);
  }

  // The node's own Texture, or a port a connection takes to another node's. A format
  // the loader does not know is the image word's mistake, which check_images names.
  std::uint32_t texture_index(std::string_view port, std::string_view pixel) override {
    if (const Field *const field = _bound.field(port); field && !field->texture())
      _bound.errors.push_back(std::format(
          "the operator fills image {}, which its shader declares as no Texture", port));
    const std::vector<ImagePort> &words = _bound.node->images;
    const auto word = std::ranges::find(words, port, &ImagePort::port);
    const PixelFormat *const format =
        pixel_format(word == words.end() ? default_format : word->format);
    if (format && format->pixel != pixel)
      _bound.errors.push_back(
          std::format("the operator fills {} with {} pixels, but its format {} takes {}",
                      port,
                      pixel,
                      format->name,
                      format->pixel));
    else if (format && !_schedule._wiring.resources.samples(format->format))
      _bound.errors.push_back(
          std::format("{}: this GPU cannot sample {} images", port, format->name));
    _bound.textures.push_back({.port = std::string(port), .format = format});
    return static_cast<std::uint32_t>(_bound.textures.size() - 1);
  }

  void check_element(const Field &field, std::string_view verb, const Element &element) {
    for (std::string &disagreement :
         disagreements(element_field(field.name, element), field, verb))
      _bound.errors.push_back(std::move(disagreement));
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

  // The writer binds before its readers, so the object exists before any reads it. One
  // from the schedule replaced stays, contents included, while its module does.
  void *output_object(std::string_view port, const Kind &kind) override {
    if (!cpp_port(port, "output"))
      return stand_in(kind);
    if (type_name(kind.type).find("anonymous namespace") != std::string::npos) {
      _bound.errors.push_back(
          std::format("output {} is a {}, in an unnamed namespace, which no other node "
                      "can name; give it a contract (RV05)",
                      port,
                      type_name(kind.type)));
      return stand_in(kind);
    }
    _bound.outputs.emplace_back(port);
    const std::string name = _schedule.buffer_name(_bound.node->name, port);
    const std::string module = module_of(_schedule._view, *_bound.node);
    if (const Held *const held = _schedule.held(name)) {
      if (held->holds(kind))
        return held->object.get();
      _bound.errors.push_back(std::format("output {} is asked for as two types", port));
      return stand_in(kind);
    }
    if (_replaced) {
      std::vector<Held> &old = _replaced->_objects;
      const auto kept = std::ranges::find_if(old, [&](const Held &held) {
        return held.name == name && held.module == module && held.holds(kind);
      });
      if (kept != old.end()) {
        Held &taken = _schedule._objects.emplace_back(std::move(*kept));
        old.erase(kept);
        return taken.object.get();
      }
    }
    return _schedule._objects
        .emplace_back(Held{.name = name,
                           .module = module,
                           .type = kind.type,
                           .size = kind.size,
                           .align = kind.align,
                           .object = kind.make()})
        .object.get();
  }

  const void *input_object(std::string_view port, const Kind &kind) override {
    if (!cpp_port(port, "input"))
      return stand_in(kind);
    _bound.inputs.emplace_back(port);
    const Connection *const connection = _schedule.connection_of(_bound.node->name, port);
    const Held *const held = connection ? _schedule.held(connection->name) : nullptr;
    if (!connection)
      _bound.errors.push_back(
          std::format("the operator reads {}, which no connection reaches: connect "
                      "another node's output to it",
                      port));
    else if (!held)
      _bound.errors.push_back(std::format("connection {}: {} gives no C++ output {}",
                                          connection->name,
                                          connection->from.node,
                                          connection->from.port));
    else if (!held->holds(kind))
      _bound.errors.push_back(
          std::format("input {} is a {} of {} bytes, but {} writes a {} of {} bytes",
                      port,
                      type_name(kind.type),
                      kind.size,
                      connection->from.node,
                      type_name(held->type.c_str()),
                      held->size));
    else
      return held->object.get();
    return stand_in(kind);
  }

  // A port is a buffer of the node's shaders or a C++ object, never both.
  bool cpp_port(std::string_view port, std::string_view what) {
    if (!_bound.field(port))
      return true;
    _bound.errors.push_back(
        std::format("{} is in its shader's pass block, so it is no C++ {}", port, what));
    return false;
  }

  void *stand_in(const Kind &kind) {
    return _bound.stand_ins.emplace_back(kind.make()).get();
  }

  Schedule &_schedule;
  Bound &_bound;
  Schedule *const _replaced;
  Commands &_commands;
  Ports &_ports;
};

// What an operator reaches during a frame: its node's block and read-backs, nothing else.
class Schedule::Cooker final : public Cook {
public:
  Cooker(Schedule &schedule, Bound &bound, std::uint64_t frame, glm::uvec2 resolution)
      : _schedule(schedule), _bound(bound), _frame(frame), _resolution(resolution) {}

private:
  std::uint64_t index() const override {
    return _frame;
  }
  glm::uvec2 resolution() const override {
    return _resolution;
  }
  void log(Level level, std::string_view text) const override {
    _schedule.log(level, Tag::out, _bound, text);
  }
  CommandPort &commands() override {
    return _schedule._wiring.commands;
  }
  InputPort &input() override {
    return _schedule._wiring.ports;
  }
  TerminalPort &terminal() override {
    return _schedule._wiring.ports;
  }
  FilePort &files() override {
    return _schedule._wiring.ports;
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
  std::span<std::byte> upload_bytes(std::uint32_t index,
                                    std::optional<std::size_t> count) override {
    const Bound::Written &upload = _bound.uploads.at(index);
    const std::span<std::byte> bytes = upload.buffer->bytes();
    const std::size_t holds = bytes.size() / upload.stride;
    if (count > holds)
      throw std::runtime_error(std::format("the operator writes {} elements of {}, which "
                                           "holds {}; upload<T>(port, count) makes room",
                                           *count,
                                           upload.port,
                                           holds));
    *upload.used = static_cast<std::uint32_t>(count.value_or(holds));
    return bytes.first(*upload.used * upload.stride);
  }
  void upload_image(std::uint32_t index,
                    std::span<const std::byte> pixels,
                    glm::uvec2 size) override {
    _schedule.upload(_bound, index, pixels, {size.x, size.y});
  }
  bool image_empty(std::uint32_t index) const override {
    const std::string name =
        std::format("{}.{}", _bound.node->name, _bound.textures.at(index).port);
    return std::ranges::find(_schedule._images, name, &Picture::name) ==
           _schedule._images.end();
  }

  Schedule &_schedule;
  Bound &_bound;
  const std::uint64_t _frame;
  const glm::uvec2 _resolution;
};

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
  if (old && old->op && old->node->module == node.module &&
      old->node->operator_name == node.operator_name) {
    bound.op = std::move(old->op);
    log(Level::debug, Tag::nod, bound, "keeps its operator " + node.operator_name);
  } else if (!node.operator_name.empty()) {
    try {
      bound.op = _wiring.modules.make(module_of(_view, node), folder, node.operator_name);
      log(Level::info, Tag::nod, bound, "new operator " + node.operator_name);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
    }
  }
  if (bound.op) {
    Binder binder(*this, bound, replaced);
    try {
      bound.op->bind(binder);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
    }
  }
  check_fields(bound);
  return bound;
}

void Schedule::drop_operators(const std::vector<std::string> &modules) {
  const auto going = [&](const std::string &module) {
    return std::ranges::find(modules, module) != modules.end();
  };
  for (Bound &bound : _bound)
    if (going(module_of(_view, *bound.node))) {
      drop_commands(bound);
      bound.op.reset();
      bound.stand_ins.clear();
    }
  // An object's destructor is code of the module that made it; the writer makes it
  // again as it binds (native C++, rule 2).
  std::erase_if(_objects, [&](const Held &held) {
    if (!going(held.module))
      return false;
    _wiring.log.write(
        Level::info,
        Tag::mod,
        std::format("{} swapped; connection {} starts empty", held.module, held.name));
    return true;
  });
}

void Schedule::cook(std::uint64_t frame, VkExtent2D resolution) {
  size_targets(resolution);
  for (const auto &entry : _buffers)
    if (entry.second.memory() == Memory::readback)
      entry.second.invalidate();
  for (Bound &bound : _bound) {
    if (!bound.errors.empty() || !bound.op)
      continue;
    Cooker cooker(*this, bound, frame, {resolution.width, resolution.height});
    try {
      bound.op->cook(cooker);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
      drop_commands(bound);
      _wiring.log.write(Level::error,
                        Tag::nod,
                        std::format("node {}: {}; its operator stops",
                                    bound.node->name,
                                    failure.what()));
    }
    if (bound.block)
      bound.block->flush();
    for (const Bound::Written &upload : bound.uploads)
      upload.buffer->flush();
  }
}

void Schedule::drop_commands(Bound &bound) {
  for (const Command command : bound.commands)
    _wiring.commands.remove(command);
  bound.commands.clear();
}

void Schedule::close_files(Bound &bound) {
  for (const File file : bound.files)
    _wiring.ports.close(file);
  bound.files.clear();
}

} // namespace VP
