#pragma once

#include "contracts/Item.h"
#include "contracts/Relation.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: which layers of curves the graph
// shows, as its relations param names them, and the buttons that turn each on and off.
namespace {

// The layers shown, a bit each, by VP_VIEW::Layer.
using Shown = std::uint32_t;
constexpr Shown every_layer = (Shown{1} << VP_VIEW::layer_names.size()) - 1;
constexpr std::string_view no_layer = "none";

constexpr Shown bit_of(VP_VIEW::Layer layer) {
  return Shown{1} << static_cast<std::size_t>(layer);
}

// The layers a relations param names, joined by commas, or none at all; a word that is no
// layer is a mistake, named (A02).
Shown layers_of(std::string_view text) {
  if (text == no_layer)
    return 0;
  Shown shown = 0;
  for (std::size_t at = 0; at <= text.size();) {
    const std::size_t end = std::min(text.find(',', at), text.size());
    const std::string_view word = text.substr(at, end - at);
    std::size_t layer = 0;
    while (layer < VP_VIEW::layer_names.size() && VP_VIEW::layer_names[layer] != word)
      ++layer;
    if (layer == VP_VIEW::layer_names.size())
      throw std::runtime_error(
          std::format("param relations = {}: {} is no layer; the layers are flow, "
                      "structure, code, build and docs, joined by commas, or none",
                      text,
                      word));
    shown |= Shown{1} << layer;
    at = end + 1;
  }
  return shown;
}

// The layers as the param names them.
std::string text_of(Shown shown) {
  std::string text;
  for (std::size_t layer = 0; layer < VP_VIEW::layer_names.size(); ++layer)
    if (shown & (Shown{1} << layer))
      text.append(text.empty() ? "" : ",").append(VP_VIEW::layer_names[layer]);
  return text.empty() ? std::string(no_layer) : text;
}

// A button a layer, lit while it shows, whose press sends the line turning it on or off,
// as the param that a save keeps and a replay restores (V08), and with every layer on,
// unsets it, as it was.
void buttons(VP_VIEW::Items &items, Shown shown, std::string_view view, std::string_view node) {
  items.clear();
  for (std::size_t layer = 0; layer < VP_VIEW::layer_names.size(); ++layer) {
    const Shown turned = shown ^ (Shown{1} << layer);
    items.push_back(
        {.label = std::string(VP_VIEW::layer_names[layer]),
         .command = turned == every_layer
                        ? std::format("{}: param unset {} relations", view, node)
                        : std::format(
                              "{}: param set {} relations {}", view, node, text_of(turned)),
         .current = (shown & (Shown{1} << layer)) != 0});
  }
}

} // namespace
