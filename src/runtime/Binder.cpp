#include "runtime/Schedule.h"

#include "runtime/Bound.h"

#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Modules.h"
#include "runtime/Ports.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
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

// Whether a view's nodes wait for a live build that has not made their code yet: a view
// this one hosts, in a run with one. The view vulpen started with fails loud as ever. One
// branch each, so a build with no live build has no code it never reaches.
// The log word of the node, or else of the nearest node it is inside that has one (V09),
// as ui sets it for ui.panel; empty for none, the process level then holding. A word that
// names no level counts as none here, and its own node names it.
std::string_view log_word(const View &view, std::string_view name) {
  for (;;) {
    const auto node = std::ranges::find(view.nodes, name, &Node::name);
    if (node != view.nodes.end() && level_named(node->log))
      return node->log;
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
      return {};
    name = name.substr(0, dot);
  }
}

bool waits_for_build([[maybe_unused]] std::string_view view) {
  if constexpr (VP_LIVE) {
    return !view.empty();
  } else {
    return false;
  }
}

// Whether the build has yet to make some of a node's code, its module or a shader's
// SPIR-V: all of it for a node of a project loaded before it was ever built, or a file
// new beside what it built, as a .geom new beside a draw's .vert and .frag.
bool unbuilt(const Node &node,
             const std::string &module,
             const std::filesystem::path &folder,
             const Modules &modules) {
  if (runs_operator(node) && !modules.built(module, folder))
    return true;
  return std::ranges::any_of(node.files, [&](const std::string &file) {
    std::error_code unread;
    return std::ranges::any_of(stages, [&](auto stage) { return file.ends_with(stage); }) &&
           !std::filesystem::exists(spirv_of(folder, file), unread);
  });
}

// What makes a connection's end a port of its node where the end names none it writes,
// or reads, the line's own mistake: a port its shaders declare nothing for yet, in the
// type the other end takes, a Texture for an image and an Image a dispatch stores into;
// or a reader named first, as the writer goes first, the line that names the two the
// other way round. Empty where neither is known, as for a draw, which writes no Image,
// or for a node with ports that way already, whose list names the one likely meant.
std::string end_fix(std::span<const NodePort> ports,
                    const View &view,
                    const Connection &connection,
                    const Endpoint &end,
                    const Bound &node) {
  const auto port_of = [&](const Endpoint &at) -> const NodePort * {
    const auto found = std::ranges::find_if(ports, [&](const NodePort &port) {
      return port.kind != "value" && port.node == at.node && port.name == at.port;
    });
    return found == ports.end() ? nullptr : &*found;
  };
  const bool writes = &end == &connection.from;
  const Endpoint *const other =
      !writes ? &connection.from : connection.to.empty() ? nullptr : &connection.to.front();
  const NodePort *const port = port_of(end);
  const bool none_that_way = std::ranges::none_of(ports, [&](const NodePort &each) {
    return each.node == end.node && each.kind != "value" && each.writes == writes;
  });
  if (!port && none_that_way &&
      (writes ? holds_stage(*node.node, compute_stage) : !node.shaders_named.empty())) {
    const NodePort *const known = other && !other->view() ? port_of(*other) : nullptr;
    return other && (other->view() || (known && known->kind == "image"))
               ? std::format("{} has no port {} yet: declare {} {}; in its pass block",
                             end.node,
                             end.port,
                             writes ? image_type : texture_type,
                             end.port)
               : std::format("{} has no port {} yet: declare it in its pass block",
                             end.node,
                             end.port);
  }
  const std::string reader = end.text();
  if (port && writes && other && connection.to.size() == 1 &&
      std::ranges::find(port->alike, other->text()) != port->alike.end())
    return std::format("connection {}: {} is a port its node reads, not one it writes: "
                       "the writer goes first, {}",
                       connection.name,
                       reader,
                       joining(view, other->text(), std::span(&reader, 1)));
  return {};
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
      return _bound.commands.emplace_back(_commands.add(
          usage,
          help,
          *_bound.op,
          Primitive::no,
          _schedule._project ? std::optional<std::string_view>(_schedule._name)
                             : std::nullopt));
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
  std::string view_name() const override {
    return _schedule._name;
  }
  const Engine &engine() const override {
    return _schedule._wiring.engine;
  }
  const View &view() const override {
    _bound.asked_view = true;
    return _schedule._view;
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
    _bound.textures.push_back({.port = std::string(port),
                               .name = std::format("{}.{}", _bound.node->name, port),
                               .format = format});
    return static_cast<std::uint32_t>(_bound.textures.size() - 1);
  }

  void check_element(const Field &field, std::string_view verb, const Element &element) {
    for (std::string &disagreement :
         disagreements(element_field(field.name, element), field, verb))
      _bound.errors.push_back(std::move(disagreement));
  }

  bool sets(std::string_view name) override {
    _bound.read_by_operator.try_emplace(std::string(name));
    return _bound.param(name) != nullptr;
  }

  std::string_view param_text(std::string_view name) override {
    _bound.read_by_operator.try_emplace(std::string(name));
    if (const Param *const param = _bound.param(name))
      return param->value;
    _bound.errors.push_back(
        std::format("the operator reads param {}, which the node does not set", name));
    return {};
  }

  void param_read_as(std::string_view name, std::string_view type, bool valid) override {
    _bound.read_by_operator.insert_or_assign(std::string(name), std::string(type));
    if (const Param *const param = _bound.param(name); param && !valid)
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
    _bound.input_types.emplace_back(kind.type);
    const Connection *const connection = _schedule.connection_of(_bound.node->name, port);
    const Held *const held = connection ? _schedule.held(connection->name) : nullptr;
    const Bound *const writer = connection ? _schedule.find(connection->from.node) : nullptr;
    if (!connection)
      _bound.errors.push_back(
          std::format("the operator reads {}, which no connection reaches: connect "
                      "another node's output to it",
                      port));
    else if (!held && writer && writer->unbuilt) // check_connections says it waits
      return stand_in(kind);
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
  void hold(std::string_view what) override {
    _schedule._wiring.commands.hold(_bound.node->name, what);
  }
  void hold_pointer() override {
    _schedule._wiring.ports.hold_pointer(_schedule._name, _bound.node->name);
  }
  CommandPort &commands() override {
    return _schedule._wiring.commands;
  }
  InputPort &input() override {
    return _schedule._wiring.ports.input(_schedule._name, _bound.node->name);
  }
  TerminalPort &terminal() override {
    _schedule._wiring.commands.reads_terminal(_schedule._name, _bound.node->name);
    return _schedule._wiring.ports;
  }
  FilePort &files() override {
    return _schedule._wiring.ports;
  }
  const View &view() const override {
    return _schedule._view;
  }
  const View *view(std::string_view name) const override {
    return _schedule._wiring.commands.view(name);
  }
  const View *unfolded(std::string_view name) const override {
    return _schedule._wiring.commands.unfolded(name);
  }
  std::span<const Cost> costs(std::string_view view) const override {
    return _schedule._wiring.commands.costs(view);
  }
  std::span<const NodePort> ports(std::string_view view) const override {
    return _schedule._wiring.commands.ports(view);
  }
  void hand_pointer(std::string_view view) override {
    _schedule._wiring.ports.hand_pointer(view, _schedule._name, _bound.node->name);
  }
  void unread(std::string_view port) override {
    _schedule._wiring.ports.unread(_schedule._name, _bound.node->name, port);
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
    return std::ranges::find(_schedule._images,
                             _bound.textures.at(index).name,
                             &Picture::name) == _schedule._images.end();
  }

  Schedule &_schedule;
  Bound &_bound;
  const std::uint64_t _frame;
  const glm::uvec2 _resolution;
};

Bound Schedule::bind(const Node &node,
                               const std::filesystem::path &folder,
                               Schedule *replaced) {
  Bound bound{.node = &node, .log = _wiring.log.level()};
  if (!node.log.empty() && !level_named(node.log))
    bound.errors.push_back(
        std::format("log = {} is not error, warn, info or debug", node.log));
  if (const std::string_view word = log_word(_view, node.name); !word.empty())
    bound.log = *level_named(word);
  Bound *const old = replaced ? replaced->find(node.name) : nullptr;
  // Its every error would be what the build has not made yet, and the view vulpen started
  // with fails loud on any, as ever, as does a view whose last build failed.
  if (waits_for_build(_name) && !_wiring.modules.build_failed(_name) &&
      unbuilt(node, module_of(_view, node), folder, _wiring.modules)) {
    bound.unbuilt = true;
    bound.errors.emplace_back(
        "is not built yet: the live build builds it, and says why if it cannot");
    return bound;
  }
  bound.invocations = counted(bound);
  load_shaders(bound, folder, old);
  // So an edit makes blocks only for the nodes it changed.
  if (old && old->block && old->block_size == bound.block_size)
    bound.block = std::move(old->block);
  // A folder whose C++ was removed runs none, though its module stays loaded till a swap.
  if (old && old->op && runs_operator(node) && old->node->module == node.module &&
      old->node->operator_name == node.operator_name) {
    bound.op = std::move(old->op);
    log(Level::debug, Tag::nod, bound, "keeps its operator");
  } else if (runs_operator(node)) {
    try {
      const std::string module = module_of(_view, node);
      const std::string name = node.operator_name.empty()
                                   ? _wiring.modules.sole(module, folder)
                                   : node.operator_name;
      bound.op = _wiring.modules.make(module, folder, name);
      log(Level::info, Tag::nod, bound, "new operator " + name);
    } catch (const Modules::Outdated &outdated) {
      bound.unbuilt = waits_for_build(_name);
      bound.errors.emplace_back(bound.unbuilt
                                    ? "was built against older engine headers than this "
                                      "vulpen's: the live build builds it again, and says "
                                      "why if it cannot"
                                    : outdated.what());
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
    }
  }
  if (bound.op) {
    Binder binder(*this, bound, replaced);
    try {
      const Log::Speaking speaking(_wiring.log, {_name, bound.node->name, bound.log});
      bound.op->bind(binder);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
      bound.cut_short = true;
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
  // again as it binds (the runtime boundary, rule 2).
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

// Against the ports and params the bind found, so a line is refused as it runs. A node
// not bound whole, as one just added, one whose operator or shaders did not load, or one
// waiting for its build, has not listed them all, and its next bind checks it instead; a
// param of a node inside it, as `command-line.host`, that node's; and a connection's end
// at a node whose operator read the view as it bound, as it may ask for that port once a
// connection names it. A buffer its shader only reads, its C++ may fill, and so write.
void Schedule::check(const View &view) const {
  const auto bound = [&](std::string_view node) -> const Bound * {
    const auto found = std::ranges::find_if(
        _bound, [&](const Bound &each) { return each.node->name == node; });
    return found != _bound.end() && found->whole() ? &*found : nullptr;
  };
  const auto listed = [&](std::string_view node, bool values) {
    std::vector<const NodePort *> ports;
    for (const NodePort &port : _ports)
      if (port.node == node && (port.kind == "value") == values)
        ports.push_back(&port);
    return ports;
  };
  const auto names = [](const std::vector<const NodePort *> &ports) {
    std::string text;
    for (const NodePort *const port : ports)
      text.append(text.empty() ? "" : ", ").append(port->name);
    return text.empty() ? std::string("none") : text;
  };
  for (const Node &node : view.nodes) {
    if (!bound(node.name))
      continue;
    const std::vector<const NodePort *> values = listed(node.name, true);
    for (const Param &param : node.params) {
      if (param.key.find('.') != std::string::npos)
        continue;
      const auto value = std::ranges::find_if(
          values, [&](const NodePort *port) { return port->name == param.key; });
      if (value == values.end())
        throw std::runtime_error(std::format("param {}: node {} reads no param of that "
                                             "name; the params it reads are {}{}",
                                             param.key,
                                             node.name,
                                             names(values),
                                             as_word(node.name, param)));
      if (!(*value)->holds.empty() && !write_value(param.value, (*value)->holds, nullptr))
        throw std::runtime_error(std::format(
            "param {} = {} is not a {}", param.key, param.value, (*value)->holds));
    }
  }
  for (const Connection &connection : view.connections)
    for (std::size_t at = 0; at <= connection.to.size(); ++at) {
      const bool writes = at == 0;
      const Endpoint &end = writes ? connection.from : connection.to[at - 1];
      const Bound *const node = end.view() ? nullptr : bound(end.node);
      if (!node || node->asked_view)
        continue;
      std::vector<const NodePort *> ports = listed(end.node, false);
      const auto port = std::ranges::find_if(
          ports, [&](const NodePort *each) { return each->name == end.port; });
      if (port != ports.end() &&
          (!writes || (*port)->writes || node->uploads_to(end.port)))
        continue;
      if (const std::string fix = end_fix(_ports, view, connection, end, *node);
          !fix.empty())
        throw std::runtime_error(fix);
      const std::string_view why = port == ports.end()
                                       ? "names no port of its node"
                                       : "is a port its node reads, not one it writes";
      std::erase_if(ports, [&](const NodePort *each) { return each->writes != writes; });
      throw std::runtime_error(std::format("connection {}: {} {}; the ports {} {} are {}",
                                           connection.name,
                                           end.text(),
                                           why,
                                           end.node,
                                           writes ? "writes" : "reads",
                                           names(ports)));
    }
}

// A dispatch's threads, as its word gives them, or as the buffer at the port it names
// holds elements: as many as the node writing it gives it, which binds before its
// readers, so a count shared along a chain is written once and no reader counts past the
// buffer's end (GLSL02).
std::uint32_t Schedule::counted(Bound &bound) const {
  const Node &node = *bound.node;
  if (node.invocations_port.empty())
    return node.invocations;
  for (const Connection &connection : _view.connections)
    if (const auto writer = _places.find(connection.from.node);
        writer != _places.end() && !connection.from.view() &&
        std::ranges::any_of(connection.to, [&](const Endpoint &to) {
          return to.node == node.name && to.port == node.invocations_port;
        })) {
      const Bound &from = _bound[writer->second];
      const Bound::Written *const upload = from.upload(connection.from.port);
      return std::max(invocations_of(from), upload ? upload->count : 0);
    }
  bound.errors.push_back(
      std::format("invocations = {}: no connection brings a buffer there from a node, "
                  "whose length would count them",
                  node.invocations_port));
  return 0;
}

// Each node's cook is timed, a node in error none; while the view rests, no node cooks.
void Schedule::cook(std::uint64_t frame, VkExtent2D resolution) {
  if (_resting)
    return;
  size_targets(resolution);
  for (const auto &entry : _buffers)
    if (entry.second.memory() == Memory::readback)
      entry.second.invalidate();
  // A node may close the view in its cook, so each asks again whether it still runs.
  for (std::size_t at = 0; at < _bound.size() && !_retired; ++at) {
    Bound &bound = _bound[at];
    Cost &cost = _costs[at];
    if (!bound.errors.empty() || !bound.op) {
      cost.cooks = false;
      continue;
    }
    if (!_pulled.empty() && !_pulled[at])
      continue;
    Cooker cooker(*this, bound, frame, {resolution.width, resolution.height});
    const auto started = std::chrono::steady_clock::now();
    try {
      const Log::Speaking speaking(_wiring.log, {_name, bound.node->name, bound.log});
      bound.op->cook(cooker);
      smooth(cost.cpu,
             cost.cpu_last,
             cost.cooks,
             std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                      started)
                 .count());
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

void Schedule::retire() {
  for (Bound &bound : _bound)
    drop_commands(bound);
  _retired = true;
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
