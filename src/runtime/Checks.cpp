#include "runtime/Schedule.h"

#include "runtime/Bound.h"

#include <algorithm>
#include <format>
#include <stdexcept>

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
    if (node.invocations != 0 || node.vertex_count != 0 || !node.instance_count.empty())
      bound.errors.emplace_back("it counts invocations or vertices, but its folder holds "
                                "no shader to run them");
  } else if (!dispatch && !draw && !fragment) {
    bound.errors.push_back(std::format("its folder holds the shaders {}, but a node runs "
                                       "one .comp, or one .vert and one .frag, or one "
                                       "of these two, its varyings joining the other",
                                       joined(shaders)));
  } else if (dispatch && (node.vertex_count != 0 || node.invocations == 0)) {
    bound.errors.emplace_back(
        "it runs a .comp, so it counts its invocations, and no vertex_count");
  } else if (draw && (node.invocations != 0 || node.vertex_count == 0)) {
    bound.errors.emplace_back(
        "it draws, so it counts its vertex_count, and no invocations");
  } else if (fragment && (node.invocations != 0 || node.vertex_count != 0 ||
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
  else if (node.invocations % size[0] != 0)
    bound.errors.push_back(
        std::format("invocations = {} is not a multiple of local_size_x = {}",
                    node.invocations,
                    size[0]));
  else if (const std::uint32_t most = _wiring.resources.max_compute_work_group_count();
           node.invocations / size[0] > most)
    bound.errors.push_back(
        std::format("invocations = {} makes {} workgroups of local_size_x = {}, more than "
                    "this GPU dispatches at once, {}",
                    node.invocations,
                    node.invocations / size[0],
                    size[0],
                    most));
}

// Before the pipeline is made, which each of these would make wrongly: a fragment node's
// shader reads the pass block of the vertex node it draws with, so it declares none; a
// draw stores into no image, as a store from a draw needs features past the Vulkan floor
// (RVK01); and a dispatch stores into one only where this GPU indexes images[].
void Schedule::check_shaders(const Bound &bound) const {
  if (is_fragment(*bound.node) && !bound.fields.empty())
    throw std::runtime_error(
        "a fragment node declares no pass block yet: the vertex node it draws with holds "
        "the draw's, and hands on what the fragment shader needs as varyings");
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
// Without its operator, or its shaders, a node cannot say what they would have read or
// set; what kept them from loading is its error.
void Schedule::check_fields(Bound &bound) const {
  if ((runs_operator(*bound.node) && !bound.op) ||
      (!bound.shaders_named.empty() && !bound.loaded()))
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
  for (Bound &bound : _bound)
    for (const Bound::Written &written : bound.uploads)
      if (!bound.field(written.port) && written.count != 0 &&
          !connection_of(bound.node->name, written.port))
        bound.errors.push_back(std::format("the operator writes {}, which nothing reads: "
                                           "connect it to a buffer of another node's "
                                           "shader",
                                           written.port));
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

} // namespace VP
