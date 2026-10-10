#include "runtime/Bound.h"

#include <algorithm>
#include <format>

namespace VP {

namespace {

std::string format_names() {
  std::string names;
  for (const PixelFormat &format : pixel_formats)
    names += (names.empty() ? "" : ", ") + std::string(format.name);
  return names;
}

std::ptrdiff_t count_stage(const std::vector<std::string> &shaders,
                           std::string_view stage) {
  return std::ranges::count_if(
      shaders, [&](const std::string &shader) { return shader.ends_with(stage); });
}

} // namespace

// What the node's shaders make it, and whether it counts what that runs: a dispatch
// its invocations, a draw its vertex_count. False when a mistake leaves nothing to run.
bool Schedule::check_counts(Bound &bound) const {
  const Node &node = *bound.node;
  const std::vector<std::string> &shaders = bound.shaders_named;
  const bool dispatch = shaders.size() == 1 && count_stage(shaders, compute_stage) == 1;
  const bool draw = shaders.size() == 2 && count_stage(shaders, vertex_stage) == 1 &&
                    count_stage(shaders, fragment_stage) == 1;
  const std::size_t before = bound.errors.size();
  if (shaders.empty()) {
    if (node.invocations != 0 || node.vertex_count != 0 || !node.instance_count.empty())
      bound.errors.emplace_back("it counts invocations or vertices, but its folder holds "
                                "no shader to run them");
  } else if (!dispatch && !draw) {
    bound.errors.push_back(std::format("its folder holds the shaders {}, but a node runs "
                                       "one .comp, or one .vert and one .frag",
                                       joined(shaders)));
  } else if (dispatch && (node.vertex_count != 0 || node.invocations == 0)) {
    bound.errors.emplace_back(
        "it runs a .comp, so it counts its invocations, and no vertex_count");
  } else if (draw && (node.invocations != 0 || node.vertex_count == 0)) {
    bound.errors.emplace_back(
        "it draws, so it counts its vertex_count, and no invocations");
  }
  return !shaders.empty() && bound.errors.size() == before;
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
    if (!field.buffer() && !field.texture() && !bound.param(field.name) &&
        !bound.set_by_operator.contains(field.name))
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
    if (std::ranges::find(writer.outputs, connection.from.port) != writer.outputs.end()) {
      check_inputs(connection);
      continue;
    }
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
    if (writer.fills(connection.from.port) || drawn) {
      sampled(connection,
              std::format("{} {}", connection.from.node, drawn ? "draws" : "fills"));
      continue;
    }
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
    for (const ImagePort &image : bound.node->images)
      if (!pixel_format(image.format))
        bound.errors.push_back(
            std::format("image {}={}: no such format; an image takes {}",
                        image.port,
                        image.format,
                        format_names()));
      else if (!bound.fills(image.port) && image.port != bound.output)
        bound.errors.push_back(std::format("image {}={}: neither its operator fills nor "
                                           "its draw renders an image {}",
                                           image.port,
                                           image.format,
                                           image.port));
  }
}

// A connection from a C++ output reaches only nodes whose operator reads it as an input.
// A node whose operator did not load says why on its own.
void Schedule::check_inputs(const Connection &connection) {
  for (const Endpoint &to : connection.to) {
    Bound &reader = *find(to.node);
    const bool unbound = !reader.op && !reader.node->operator_name.empty();
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
