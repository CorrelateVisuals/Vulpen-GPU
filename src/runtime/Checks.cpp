#include "runtime/Schedule.h"

#include "runtime/Bound.h"

#include <algorithm>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace VP {

namespace {

std::string format_names() {
  std::string names;
  for (const PixelFormat &format : pixel_formats)
    names += (names.empty() ? "" : ", ") + std::string(format.name);
  return names;
}

// One more reason a node waits for the build, rather than an error of its own.
void waits(Bound &bound, std::string why) {
  if (bound.unbuilt)
    return;
  bound.errors.push_back(std::move(why));
  ++bound.waits;
}

std::ptrdiff_t count_stage(const std::vector<std::string> &shaders,
                           std::string_view stage) {
  return std::ranges::count_if(
      shaders, [&](const std::string &shader) { return shader.ends_with(stage); });
}

// Whether a field a shader reads holds what a writer writes, as a connection between them
// is checked: a shader's buffer alike by stride and type, what C++ fills member for
// member.
bool holds_alike(const Field &written, bool by_cpp, const Field &read) {
  return by_cpp ? disagreements(written, read, "writes").empty()
                : written.stride == read.stride && written.type == read.type;
}

// The buffers no connection joins that other nodes write, holding what a loose field
// reads, each as node.port: a shader's, and one C++ fills at a port no shader of its node
// holds.
template <class Loose>
std::vector<std::string> writers_alike(std::span<const Bound> bounds,
                                       const Bound &reader,
                                       const Field &in,
                                       const Loose &loose) {
  std::vector<std::string> found;
  for (const Bound &writer : bounds) {
    if (&writer == &reader || writer.unbuilt)
      continue;
    for (const Field &out : writer.fields)
      if (out.buffer() && writes(out.access) && loose(writer, out.name) &&
          holds_alike(out, false, in))
        found.push_back(std::format("{}.{}", writer.node->name, out.name));
    for (const Bound::Written &filled : writer.uploads)
      if (filled.count != 0 && !writer.field(filled.port) && loose(writer, filled.port) &&
          holds_alike(filled.element, true, in))
        found.push_back(std::format("{}.{}", writer.node->name, filled.port));
  }
  return found;
}

// The fields no connection joins, nor their own C++ fills, that other nodes' shaders
// read, holding what a writer writes, its shader or by_cpp its C++, each as node.port.
template <class Loose>
std::vector<std::string> readers_alike(std::span<const Bound> bounds,
                                       const Bound &writer,
                                       const Field &written,
                                       bool by_cpp,
                                       const Loose &loose) {
  std::vector<std::string> found;
  for (const Bound &reader : bounds)
    for (const Field &in : reader.fields)
      if (&reader != &writer && in.buffer() && in.access == Access::read &&
          !reader.uploads_to(in.name) && loose(reader, in.name) &&
          holds_alike(written, by_cpp, in))
        found.push_back(std::format("{}.{}", reader.node->name, in.name));
  return found;
}

// A varying as its ends must share it, its location and its type, which is what a reader
// alike holds, whatever each end names it.
std::string varying_type(const Varying &varying) {
  return std::format("{} at {}", varying.type, varying.location);
}

// An image written, an object or a varying, loose, and the loose ports of other nodes
// holding it the other way round: an image reaches a Texture, whatever its format, and an
// object one of its type, as a connection checks them.
void join_alike(std::vector<NodePort> &ports) {
  const auto takes = [](const NodePort &from, const NodePort &to) {
    if (from.kind != to.kind || from.node == to.node || to.joined || to.writes)
      return false;
    if (from.kind == "image")
      return to.holds == texture_type;
    return from.kind != "buffer" && !from.holds.empty() && from.holds == to.holds;
  };
  for (NodePort &port : ports) {
    if (port.joined || port.kind == "buffer")
      continue;
    for (const NodePort &other : ports)
      if (port.writes ? takes(port, other)
                      : other.writes && !other.joined && takes(other, port))
        port.alike.push_back(std::format("{}.{}", other.node, other.name));
  }
}

// After the ports, of a kind of their own, so a part listing ports leaves them out by it:
// each value a param may set, joined while the node sets it.
void list_values(std::span<const Bound> bounds, std::vector<NodePort> &ports) {
  for (const Bound &bound : bounds) {
    const auto add = [&](std::string_view name, std::string_view holds) {
      if (std::ranges::none_of(ports, [&](const NodePort &port) {
            return port.node == bound.node->name && port.kind == "value" &&
                   port.name == name;
          }))
        ports.push_back(NodePort{.node = bound.node->name,
                                 .name = name,
                                 .kind = "value",
                                 .holds = std::string(holds),
                                 .joined = bound.param(name) != nullptr});
    };
    for (const Field &field : bound.fields)
      if (!field.buffer() && !field.texture() && !field.image() &&
          !bound.set_by_operator.contains(field.name))
        add(field.name, field.type);
    for (const auto &[read, type] : bound.read_by_operator)
      add(read, type);
  }
}

// The line that joins a writer to readers, its connection named for the writer's port,
// or while a connection of the view holds that name, for its node's and the port's.
std::string joining(const View &view,
                    std::string_view writer,
                    std::span<const std::string> readers) {
  const std::size_t dot = writer.rfind('.');
  const std::size_t before = writer.rfind('.', dot - 1);
  const std::size_t node = before == std::string_view::npos ? 0 : before + 1;
  std::string name(writer.substr(dot + 1));
  if (std::ranges::find(view.connections, name, &Connection::name) != view.connections.end())
    name = std::format("{}-{}", writer.substr(node, dot - node), name);
  std::string line = std::format("connect {} {}", name, writer);
  for (const std::string &reader : readers)
    line.append(" ").append(reader);
  return line;
}

} // namespace

// What the node's shaders make it, and whether it counts what that runs: a dispatch
// its invocations, a draw its vertex_count, as a vertex node does, and a fragment node
// nothing. False when a mistake leaves nothing to run.
bool Schedule::check_counts(Bound &bound) const {
  const Node &node = *bound.node;
  const std::vector<std::string> &shaders = bound.shaders_named;
  const bool dispatch = shaders.size() == 1 && count_stage(shaders, compute_stage) == 1;
  const bool fragment = shaders.size() == 1 && count_stage(shaders, fragment_stage) == 1;
  const bool vertex = shaders.size() == 1 && count_stage(shaders, vertex_stage) == 1;
  const bool draw = vertex || (shaders.size() == 2 &&
                               count_stage(shaders, vertex_stage) == 1 &&
                               count_stage(shaders, fragment_stage) == 1);
  const std::size_t before = bound.errors.size();
  if (shaders.empty()) {
    if (node.counts_invocations() || node.vertex_count != 0 || !node.instance_count.empty())
      bound.errors.emplace_back("it counts invocations or vertices, but its folder holds "
                                "no shader to run them");
  } else if (!dispatch && !draw && !fragment) {
    bound.errors.push_back(std::format("its folder holds the shaders {}, but a node runs "
                                       "one .comp, or one .vert and one .frag, or one "
                                       "of these two, its varyings joining the other",
                                       joined(shaders)));
  } else if (dispatch && (node.vertex_count != 0 || !node.counts_invocations())) {
    bound.errors.emplace_back(
        "it runs a .comp, so it counts its invocations, and no vertex_count");
  } else if (dispatch && bound.invocations == 0) {
    return false; // its port counted none, as counted said, and it makes no buffers
  } else if (draw && (node.counts_invocations() || node.vertex_count == 0)) {
    bound.errors.emplace_back(
        "it draws, so it counts its vertex_count, and no invocations");
  } else if (fragment && (node.counts_invocations() || node.vertex_count != 0 ||
                          !node.instance_count.empty())) {
    bound.errors.emplace_back(
        "a fragment node counts nothing: the vertex node it draws with counts");
  }
  return !shaders.empty() && bound.errors.size() == before;
}

// A draw's shaders only read buffers: storing from them needs features beyond the
// Vulkan floor (RVK01).
void Schedule::check_stages(Bound &bound) const {
  const Node &node = *bound.node;
  if (is_fragment(node))
    return;
  if (is_draw(node)) {
    for (const Field &field : bound.fields)
      if (field.buffer() && field.access != Access::read)
        bound.errors.push_back(std::format(
            "{}: a draw's shaders only read buffers; declare it readonly", field.name));
    if (const Field *const field = bound.field(node.instance_count);
        !instance_number(node) && (!field || !field->buffer()))
      bound.errors.push_back(
          std::format("instance_count = {}: its shaders hold no buffer of that name, "
                      "whose used length would count the instances",
                      node.instance_count));
    return;
  }
  if (!node.instance_count.empty())
    bound.errors.emplace_back(
        "instance_count counts a draw's instances; a dispatch has none");
  const std::array<std::uint32_t, 3> &size = bound.shaders.front().workgroup_size();
  if (size[1] != 1 || size[2] != 1)
    bound.errors.push_back("only one-dimensional workgroups run yet: local_size_y and "
                           "local_size_z must be 1");
  else if (bound.invocations % size[0] != 0)
    bound.errors.push_back(
        std::format("invocations = {} is not a multiple of local_size_x = {}",
                    bound.invocations,
                    size[0]));
  else if (const std::uint32_t most = _wiring.resources.max_compute_work_group_count();
           bound.invocations / size[0] > most)
    bound.errors.push_back(
        std::format("invocations = {} makes {} workgroups of local_size_x = {}, more than "
                    "this GPU dispatches at once, {}",
                    bound.invocations,
                    bound.invocations / size[0],
                    size[0],
                    most));
}

// Before the pipeline is made, which each of these would make wrongly: a fragment node's
// shader reads the pass block of the vertex node it draws with, so it declares none; a
// node's own .frag reads only what its .vert writes, at the same location with the same
// type, as check_varyings asks of a draw over two nodes, so none reads what nothing wrote
// (GLSL02); a draw stores into no image, as a store from a draw needs features past the
// Vulkan floor (RVK01); and a dispatch stores into one only where this GPU indexes
// images[].
void Schedule::check_shaders(const Bound &bound) const {
  if (is_fragment(*bound.node) && !bound.fields.empty())
    throw std::runtime_error(
        "a fragment node declares no pass block yet: the vertex node it draws with holds "
        "the draw's, and hands on what the fragment shader needs as varyings");
  if (is_draw(*bound.node) && !is_vertex(*bound.node)) {
    const std::vector<Varying> &outputs = bound.stage(vertex_stage).outputs();
    for (const Varying &input : bound.stage(fragment_stage).inputs()) {
      const auto output = std::ranges::find(outputs, input.location, &Varying::location);
      if (output == outputs.end())
        throw std::runtime_error(std::format(
            "{}: its .frag reads a {} at location {}, which its .vert writes nothing to",
            input.name,
            input.type,
            input.location));
      if (output->type != input.type)
        throw std::runtime_error(std::format("{}: its .frag reads a {} at location {}, "
                                             "but its .vert writes {} there as a {}; a "
                                             "varying's ends share both",
                                             input.name,
                                             input.type,
                                             input.location,
                                             output->name,
                                             output->type));
    }
  }
  const auto image = std::ranges::find_if(bound.fields, &Field::image);
  if (image == bound.fields.end())
    return;
  if (is_draw(*bound.node))
    throw std::runtime_error(
        std::format("{}: a draw's shaders store into no image, as that needs features "
                    "past the Vulkan floor; a dispatch stores into one",
                    image->name));
  if (!_wiring.resources.samples(pixel_format(stored_format)->format, Fill::store))
    throw std::runtime_error(
        std::format("{}: this GPU cannot store into images, as it cannot index "
                    "images[] (shaderStorageImageArrayDynamicIndexing)",
                    image->name));
}

// A vertex node draws with the fragment node its varyings reach, once both are bound, as
// one draw: its own pass, of its pass block and counts, and the fragment node's color. It
// keeps its pipeline while the build left both shaders alone and it fits what it draws
// into. A fragment node no vertex node draws with is refused, as it would draw nothing.
void Schedule::pair_draws(Schedule *replaced) {
  for (Bound &bound : _bound) {
    if (!is_vertex(*bound.node) || !bound.loaded())
      continue;
    bound.fragment = fragment_of(bound);
    if (!bound.fragment)
      continue;
    Bound *const old = replaced ? replaced->find(bound.node->name) : nullptr;
    const bool unchanged = old && old->fragment && old->spirv == bound.spirv &&
                           old->built == bound.built &&
                           old->fragment->spirv == bound.fragment->spirv &&
                           old->fragment->built == bound.fragment->built;
    try {
      fit_pipeline(bound, old, unchanged);
    } catch (const std::exception &failure) {
      bound.errors.emplace_back(failure.what());
    }
  }
  // A vertex node its varyings come from that the build has not made yet draws with it
  // once built.
  const auto unbuilt_writer = [&](const Bound &reader) -> const Bound * {
    for (const Connection &connection : _view.connections) {
      const Bound *const writer = find(connection.from.node);
      if (writer && writer->unbuilt &&
          std::ranges::find(connection.to, reader.node->name, &Endpoint::node) !=
              connection.to.end())
        return writer;
    }
    return nullptr;
  };
  for (Bound &bound : _bound)
    if (is_fragment(*bound.node) && bound.loaded() && bound.errors.empty() &&
        std::ranges::none_of(_bound, [&](const Bound &vertex) {
          return vertex.fragment == &bound;
        })) {
      if (const Bound *const vertex = unbuilt_writer(bound))
        waits(bound,
              std::format("waits for the vertex node {}, which the live build builds",
                          vertex->node->name));
      else
        bound.errors.emplace_back("no vertex node draws with it: a fragment node draws "
                                  "with the vertex node whose varyings reach its inputs");
    }
}

// The fragment node a vertex node's varyings reach, which makes one draw with it, or
// null, with why among the errors of either.
Bound *Schedule::fragment_of(Bound &vertex) {
  const std::vector<Varying> &outputs = vertex.stage(vertex_stage).outputs();
  std::vector<const Connection *> varyings;
  Bound *fragment = nullptr;
  for (const Connection &connection : _view.connections) {
    if (connection.from.node != vertex.node->name ||
        std::ranges::find(outputs, connection.from.port, &Varying::name) == outputs.end())
      continue;
    varyings.push_back(&connection);
    for (const Endpoint &to : connection.to) {
      Bound *const reader = find(to.node);
      if (fragment && reader != fragment) {
        vertex.errors.push_back(std::format("its varyings reach {} and {}, but a draw "
                                            "has one fragment node",
                                            fragment->node->name,
                                            to.node));
        return nullptr;
      }
      fragment = reader;
    }
  }
  if (!fragment || !is_fragment(*fragment->node)) {
    vertex.errors.push_back(
        fragment ? std::format("its varyings reach {}, whose folder holds no lone .frag "
                               "to draw with",
                               fragment->node->name)
                 : "its varyings reach no fragment node: connect its outputs to the "
                   "inputs of a node whose folder holds a lone .frag");
    return nullptr;
  }
  if (std::ranges::any_of(_bound, [&](const Bound &other) {
        return other.fragment == fragment;
      })) {
    vertex.errors.push_back(std::format("{} draws with another vertex node already; a "
                                        "draw has one",
                                        fragment->node->name));
    return nullptr;
  }
  if (fragment->loaded())
    check_varyings(vertex, *fragment, varyings);
  if (fragment->unbuilt) {
    waits(vertex,
          std::format("waits for its fragment node {}, which the live build builds",
                      fragment->node->name));
    return nullptr;
  }
  if (!fragment->errors.empty()) {
    vertex.errors.push_back(
        std::format("its fragment node {} is left out", fragment->node->name));
    return nullptr;
  }
  return fragment;
}

// The stages match a varying by its location, so both ends sit at the same one with the
// same type, whatever the connection names them; and every input of the fragment shader
// comes through a connection, so none reads what nothing wrote (GLSL02).
void Schedule::check_varyings(const Bound &vertex,
                              Bound &fragment,
                              const std::vector<const Connection *> &varyings) const {
  const std::vector<Varying> &outputs = vertex.stage(vertex_stage).outputs();
  const std::vector<Varying> &inputs = fragment.stage(fragment_stage).inputs();
  for (const Connection *const connection : varyings) {
    const Varying &out =
        *std::ranges::find(outputs, connection->from.port, &Varying::name);
    for (const Endpoint &to : connection->to) {
      const auto in = std::ranges::find(inputs, to.port, &Varying::name);
      if (in == inputs.end())
        fragment.errors.push_back(std::format(
            "connection {}: {} is no input of its .frag", connection->name, to.port));
      else if (in->location != out.location || in->type != out.type)
        fragment.errors.push_back(std::format("connection {}: {} is a {} at location {}, "
                                              "but {} is a {} at location {}; a "
                                              "varying's ends share both",
                                              connection->name,
                                              connection->from.text(),
                                              out.type,
                                              out.location,
                                              to.text(),
                                              in->type,
                                              in->location));
    }
  }
  for (const Varying &input : inputs)
    if (std::ranges::none_of(varyings, [&](const Connection *connection) {
          return std::ranges::any_of(connection->to, [&](const Endpoint &to) {
            return to.port == input.name;
          });
        }))
      fragment.errors.push_back(std::format(
          "{}: no connection brings it from {}", input.name, vertex.node->name));
}

// Every param is read by someone, and every value in the pass block is set by someone.
// Without its operator or its shaders, or with its operator's bind cut short, a node
// cannot say what they would have read or set; what kept them is its error.
void Schedule::check_fields(Bound &bound) const {
  if (!bound.whole())
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
    if (!field.buffer() && !field.texture() && !field.image() &&
        !bound.param(field.name) && !bound.set_by_operator.contains(field.name))
      bound.errors.push_back(std::format("{} in the pass block: nothing sets it; give "
                                         "the node a param {} or set it from "
                                         "the operator",
                                         field.name,
                                         field.name));
}

void Schedule::check_connections() {
  // An image reaches only Textures. A reader whose shaders did not load says why on its
  // own; one with none has no Texture to sample.
  const auto sampled = [&](const Connection &connection, std::string_view drawn) {
    for (const Endpoint &to : connection.to) {
      Bound &reader = *find(to.node);
      const Field *const in = reader.field(to.port);
      if ((reader.loaded() || reader.shaders_named.empty()) && (!in || !in->texture()))
        reader.errors.push_back(
            std::format("connection {}: {} an image, which {} is no Texture to sample",
                        connection.name,
                        drawn,
                        to.port));
    }
  };
  for (const Connection &connection : _view.connections) {
    if (connection.from.view()) {
      sampled(connection, std::format("view {} draws", connection.from.node));
      continue;
    }
    Bound &writer = *find(connection.from.node);
    // Its readers wait for a writer the build has not made yet, which the build starts.
    if (writer.unbuilt) {
      for (const Endpoint &to : connection.to)
        waits(*find(to.node),
              std::format("connection {}: waits for {}, which the live build builds",
                          connection.name,
                          writer.node->name));
      continue;
    }
    if (std::ranges::find(writer.outputs, connection.from.port) != writer.outputs.end()) {
      check_inputs(connection);
      continue;
    }
    // A varying, which pair_draws checked, or one of a vertex node that did not load,
    // which says why on its own.
    if (is_vertex(*writer.node) &&
        (!writer.loaded() || std::ranges::find(writer.stage(vertex_stage).outputs(),
                                               connection.from.port,
                                               &Varying::name) !=
                                 writer.stage(vertex_stage).outputs().end()))
      continue;
    // A buffer C++ fills at a port no shader of its node holds reaches shaders that hold
    // its element as C++ writes it.
    const Bound::Written *const filled = writer.upload(connection.from.port);
    if (filled && !writer.field(connection.from.port)) {
      for (const Endpoint &to : connection.to) {
        Bound &reader = *find(to.node);
        const Field *const in = reader.field(to.port);
        if (!reader.loaded() && !reader.shaders_named.empty())
          continue;
        if (filled->count == 0)
          reader.errors.push_back(
              std::format("connection {}: nothing writes it", connection.name));
        else if (!in || !in->buffer() || !reads(in->access))
          reader.errors.push_back(
              std::format("connection {}: {} is not a buffer its shader reads",
                          connection.name,
                          to.port));
        else
          for (const std::string &disagreement :
               disagreements(filled->element, *in, "writes"))
            reader.errors.push_back(
                std::format("connection {}: {}", connection.name, disagreement));
      }
      continue;
    }
    const bool drawn = writer.drawn && connection.from.port == writer.output;
    const Field *const out = writer.field(connection.from.port);
    if (writer.fills(connection.from.port) || drawn || (out && out->image())) {
      sampled(connection,
              std::format("{} {}",
                          connection.from.node,
                          drawn ? "draws" : out && out->image() ? "stores" : "fills"));
      continue;
    }
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
  // A port no connection joins names the line that joins it, where another node's port
  // holds the same the other way round (A02): what to type, beside what is wrong.
  const auto loose = [&](const Bound &bound, std::string_view port) {
    return !connection_of(bound.node->name, port);
  };
  for (Bound &bound : _bound)
    for (const Field &field : bound.fields) {
      if (!field.buffer() || field.access != Access::read)
        continue;
      const Connection *const connection = connection_of(bound.node->name, field.name);
      const bool uploaded = bound.uploads_to(field.name);
      const std::vector<std::string> writers =
          connection || uploaded ? std::vector<std::string>()
                                 : writers_alike(_bound, bound, field, loose);
      const std::string reader = std::format("{}.{}", bound.node->name, field.name);
      if (!connection && !uploaded && writers.empty())
        bound.errors.push_back(
            std::format("{} reads nothing: connect it to a buffer another node writes, "
                        "or write it from the operator",
                        field.name));
      else if (!connection && !uploaded)
        bound.errors.push_back(std::format(
            "{} reads nothing: {}{}",
            field.name,
            joining(_view, writers.front(), std::span(&reader, 1)),
            writers.size() == 1
                ? std::string()
                : std::format(", or from {}, which write it alike",
                              joined({writers.begin() + 1, writers.end()}))));
      else if (connection && uploaded && connection->from.node != bound.node->name)
        bound.errors.push_back(
            std::format("{} is written both by the operator and through connection {}",
                        field.name,
                        connection->name));
    }
  for (Bound &bound : _bound)
    for (const Bound::Written &written : bound.uploads) {
      if (bound.field(written.port) || written.count == 0 ||
          connection_of(bound.node->name, written.port))
        continue;
      const std::vector<std::string> readers =
          readers_alike(_bound, bound, written.element, true, loose);
      bound.errors.push_back(
          readers.empty()
              ? std::format("the operator writes {}, which nothing reads: connect it to "
                            "a buffer of another node's shader",
                            written.port)
              : std::format(
                    "the operator writes {}, which nothing reads: {}",
                    written.port,
                    joining(_view,
                            std::format("{}.{}", bound.node->name, written.port),
                            readers)));
    }
}

// Every image a node's C++ fills is sampled, by its own shaders or through a connection;
// else its C++ would fill it for nothing. A Texture nothing reaches samples nothing, as
// it does before its image is filled or drawn: it may wait for a command to connect it,
// as the ide's waits for the view it presents.
void Schedule::check_images() {
  for (Bound &bound : _bound) {
    if (bound.unbuilt)
      continue;
    for (const Field &field : bound.fields) {
      if (!field.texture())
        continue;
      const Connection *const connection = connection_of(bound.node->name, field.name);
      if (connection && bound.fills(field.name) &&
          connection->from.node != bound.node->name)
        bound.errors.push_back(
            std::format("{} is filled both by the operator and through connection {}",
                        field.name,
                        connection->name));
    }
    for (const Bound::Filled &filled : bound.textures)
      if (!bound.field(filled.port) && !connection_of(bound.node->name, filled.port))
        bound.errors.push_back(std::format("the operator fills image {}, which nothing "
                                           "samples: no Texture of its shaders, and no "
                                           "connection",
                                           filled.port));
    for (const ImagePort &image : bound.node->images) {
      const Field *const field = bound.field(image.port);
      const bool stored = field && field->image();
      if (!pixel_format(image.format))
        bound.errors.push_back(
            std::format("image {}={}: no such format; an image takes {}",
                        image.port,
                        image.format,
                        format_names()));
      else if (stored && image.format != stored_format)
        bound.errors.push_back(
            std::format("image {}={}: a dispatch stores into {} images only, as "
                        "images[] declares them",
                        image.port,
                        image.format,
                        stored_format));
      else if (!bound.fills(image.port) && image.port != bound.output && !stored)
        bound.errors.push_back(std::format("image {}={}: neither its operator fills, "
                                           "its draw renders nor its dispatch stores an "
                                           "image {}",
                                           image.port,
                                           image.format,
                                           image.port));
    }
  }
}

// A connection from a C++ output reaches only nodes whose operator reads it as an input.
// A node whose operator did not load says why on its own.
void Schedule::check_inputs(const Connection &connection) {
  for (const Endpoint &to : connection.to) {
    Bound &reader = *find(to.node);
    const bool unbound = !reader.op && runs_operator(*reader.node);
    if (!unbound && std::ranges::find(reader.inputs, to.port) == reader.inputs.end())
      reader.errors.push_back(
          std::format("connection {}: {} writes a C++ object, which {} does not read as "
                      "an input",
                      connection.name,
                      connection.from.node,
                      to.port));
  }
}

// Each node's ports as its bind found them, for parts to read as they read costs (V05):
// its pass block's buffers and images, a buffer or an image its C++ fills at a port no
// shader holds, a draw's color, its C++ objects, and a split draw's varyings; a loose
// one with the loose ports of other nodes that would join it, as a buffer's error names
// them. Then each value a param may set: one of its pass block that its C++ does not set,
// and one its C++ reads, joined while the node sets it.
void Schedule::list_ports() {
  const auto loose = [&](const Bound &bound, std::string_view port) {
    return !connection_of(bound.node->name, port);
  };
  for (const Bound &bound : _bound) {
    const auto add = [&](std::string_view name,
                         std::string_view kind,
                         bool written,
                         std::string_view holds) -> NodePort & {
      return _ports.emplace_back(NodePort{.node = bound.node->name,
                                          .name = name,
                                          .kind = kind,
                                          .writes = written,
                                          .holds = std::string(holds),
                                          .joined = !loose(bound, name)});
    };
    for (const Field &field : bound.fields)
      if (field.buffer()) {
        NodePort &port = add(field.name, "buffer", writes(field.access), field.type);
        port.joined = port.joined || (!port.writes && bound.uploads_to(field.name));
        if (!port.joined)
          port.alike = port.writes ? readers_alike(_bound, bound, field, false, loose)
                                   : writers_alike(_bound, bound, field, loose);
      } else if (field.texture() || field.image()) {
        // An Image is what the dispatch stores into, as make_targets has it: its field
        // holds the image's handle, which the block only reads.
        add(field.name, "image", field.image(), field.type);
      }
    for (const Bound::Written &filled : bound.uploads)
      if (!bound.field(filled.port) && filled.count != 0)
        if (NodePort &port = add(filled.port, "buffer", true, filled.element.type);
            !port.joined)
          port.alike = readers_alike(_bound, bound, filled.element, true, loose);
    for (const Bound::Filled &filled : bound.textures)
      if (!bound.field(filled.port))
        add(filled.port, "image", true, filled.format ? filled.format->name : "");
    if (!bound.output.empty())
      add(bound.output, "image", true, bound.drawn ? bound.drawn->name : "");
    for (const std::string &output : bound.outputs) {
      const Held *const held = this->held(buffer_name(bound.node->name, output));
      add(output, "object", true, held ? held->type : "");
    }
    for (std::size_t at = 0; at < bound.inputs.size(); ++at)
      add(bound.inputs[at], "object", false, bound.input_types[at]);
    if (bound.loaded() && is_vertex(*bound.node))
      for (const Varying &varying : bound.stage(vertex_stage).outputs())
        add(varying.name, "varying", true, varying_type(varying));
    if (bound.loaded() && is_fragment(*bound.node))
      for (const Varying &varying : bound.stage(fragment_stage).inputs())
        add(varying.name, "varying", false, varying_type(varying));
  }
  join_alike(_ports);
  list_values(_bound, _ports);
}


} // namespace VP
