#pragma once

#include "contracts/Item.h"
#include "contracts/Palette.h"
#include "runtime/Operator.h"
#include "Boxes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: what the heat param names, what
// each box costs, the fill its cost gives it and the tip that tells it, and the buttons
// that turn the heat on and off.
namespace {

// What the boxes show the cost of, as the heat param names it: the GPU's or the CPU's,
// against the costliest box beside it, or against a budget in milliseconds.
struct Heat {
  enum class On : std::uint8_t { none, gpu, cpu };
  On on = On::none;
  float budget = 0.0f; // none: the costliest box beside it

  bool operator==(const Heat &) const = default;
};
// By Heat::On past none, as the param and the buttons name them.
constexpr auto heat_names = std::to_array<std::string_view>({"gpu", "cpu"});
// The theme's ramp, from what costs least to what costs most.
constexpr std::array ramp{VP_VIEW::role("cold"),
                          VP_VIEW::role("cool"),
                          VP_VIEW::role("mild"),
                          VP_VIEW::role("warm"),
                          VP_VIEW::role("hot")};

// gpu or cpu, or either with a budget after a colon, as gpu:2.5; anything else is a
// mistake, named (A02).
Heat heat_of(std::string_view text) {
  const std::size_t colon = text.find(':');
  const std::string_view word = text.substr(0, colon);
  Heat heat;
  for (std::size_t on = 0; on < heat_names.size(); ++on)
    if (word == heat_names[on])
      heat.on = static_cast<Heat::On>(on + 1);
  bool fits = heat.on != Heat::On::none;
  if (colon != std::string_view::npos) {
    const std::string_view budget = text.substr(colon + 1);
    const auto [end, error] =
        std::from_chars(budget.data(), budget.data() + budget.size(), heat.budget);
    fits = fits && error == std::errc{} && end == budget.data() + budget.size() &&
           heat.budget > 0.0f;
  }
  if (!fits)
    throw std::runtime_error(
        std::format("param heat = {}: the heat is gpu or cpu, against the costliest box "
                    "beside each, or either with a budget in milliseconds after a colon, "
                    "as gpu:2.5",
                    text));
  return heat;
}

// What a box costs, the node it shows and those inside it, or each node of a view, in
// milliseconds a frame, smoothed and as the last frame took it; whether any of them cooks
// or has a pass, and whether a pass of theirs went untimed.
struct Weight {
  float cpu = 0.0f;
  float cpu_last = 0.0f;
  float gpu = 0.0f;
  float gpu_last = 0.0f;
  bool cooks = false;
  bool runs = false;
  bool untimed = false;
};

void add(Weight &weight, const VP::Cost &cost) {
  if (cost.cooks) {
    weight.cpu += cost.cpu;
    weight.cpu_last += cost.cpu_last;
  }
  if (cost.timed) {
    weight.gpu += cost.gpu;
    weight.gpu_last += cost.gpu_last;
  }
  weight.cooks = weight.cooks || cost.cooks;
  weight.runs = weight.runs || cost.runs;
  weight.untimed = weight.untimed || (cost.runs && !cost.timed);
}

float shown(const Weight &weight, Heat heat) {
  return heat.on == Heat::On::gpu ? weight.gpu : weight.cpu;
}

// Where a node of the view shown counts: in the box beside the frame whose name is its
// own, or whose name and a dot start its own, as a box shows the nodes inside its node;
// and in the frame, the node whose inside shows, if it is that node or inside it. A level
// nests no box but in the frame, so one box at most holds a node.
struct Holder {
  std::size_t box = no_box;
  bool framed = false;
};

bool holds(std::string_view name, std::string_view node) {
  return node == name || (node.starts_with(name) && node[name.size()] == '.');
}

Holder holder_of(const Boxes &boxes, std::string_view node) {
  Holder holder{.framed = boxes.frame != no_box &&
                          holds(boxes.boxes[boxes.frame].name, node)};
  for (std::size_t box = 0; box < boxes.boxes.size(); ++box)
    if (box != boxes.frame && boxes.boxes[box].kind != Kind::view &&
        holds(boxes.boxes[box].name, node))
      holder.box = box;
  return holder;
}

// What the boxes show the cost of, as the heat param names it; what each costs this
// frame, and the most of the boxes beside the frame, which the ramp's hot end stands for;
// and where each node of the view shown counts, found again only as the boxes, the view
// shown or the costs' table change, so a frame allocates nothing for it.
class Heating {
public:
  // The boxes' changed, or the view they show.
  void stale() {
    _stale = true;
  }

  // While the heat shows: the nodes of the view shown each box holds, the frame every one
  // inside it, and a view's box every node of that view. A folder's boxes are no nodes,
  // and cost nothing.
  void weigh(VP::Cook &frame, const Boxes &boxes, std::string_view view, bool folders) {
    weights.assign(boxes.boxes.size(), {});
    most = 0.0f;
    if (heat.on == Heat::On::none || folders)
      return;
    const std::span<const VP::Cost> costs = frame.costs(view);
    if (_stale || costs.data() != _from || costs.size() != _holders.size()) {
      _holders.clear();
      for (const VP::Cost &cost : costs)
        _holders.push_back(holder_of(boxes, cost.node));
      _from = costs.data();
      _stale = false;
    }
    for (std::size_t at = 0; at < costs.size(); ++at) {
      if (_holders[at].box != no_box)
        add(weights[_holders[at].box], costs[at]);
      if (_holders[at].framed)
        add(weights[boxes.frame], costs[at]);
    }
    for (std::size_t box = 0; box < boxes.boxes.size(); ++box) {
      if (boxes.boxes[box].kind == Kind::view)
        for (const VP::Cost &cost : frame.costs(boxes.boxes[box].name))
          add(weights[box], cost);
      if (box != boxes.frame)
        most = std::max(most, shown(weights[box], heat));
    }
  }

  Heat heat;
  std::vector<Weight> weights; // by box
  float most = 0.0f;

private:
  std::vector<Holder> _holders; // by cost of the view shown
  const VP::Cost *_from = nullptr;
  bool _stale = true;
};

// What fills a box shown with its cost: the two roles of the ramp its share of the most
// falls between, and how far toward the second; muted for a pass the GPU has not timed,
// which must not look cheap; the panel's, as with no heat, for a box with no cost of the
// kind shown.
struct Fill {
  std::uint32_t role = VP_VIEW::role("panel");
  std::uint32_t blend_role = 0;
  float blend = 0.0f;
};

Fill fill_of(const Weight &weight, Heat heat, float most) {
  const bool gpu = heat.on == Heat::On::gpu;
  if (heat.on == Heat::On::none || !(gpu ? weight.runs : weight.cooks))
    return {};
  if (gpu && weight.untimed && weight.gpu == 0.0f)
    return {.role = VP_VIEW::role("muted")};
  const float scale = heat.budget > 0.0f ? heat.budget : most;
  const float share =
      scale > 0.0f ? std::clamp(shown(weight, heat) / scale, 0.0f, 1.0f) : 0.0f;
  const float along = share * static_cast<float>(ramp.size() - 1);
  const std::size_t step = std::min(static_cast<std::size_t>(along), ramp.size() - 2);
  return {.role = ramp[step],
          .blend_role = ramp[step + 1],
          .blend = along - static_cast<float>(step)};
}

// A box's cost, as its tip tells it: its name, then its milliseconds a frame on the CPU
// and on the GPU, smoothed and as the last frame took them. Nothing for a box that costs
// nothing.
void tell_cost(VP_VIEW::Items &tip, std::string_view title, const Weight &weight) {
  tip.clear();
  if (!weight.cooks && !weight.runs)
    return;
  tip.push_back({.label = std::string(title)});
  if (weight.cooks)
    tip.push_back({.label = std::format("cpu {:.3f} ms a frame, {:.3f} the last frame",
                                        weight.cpu,
                                        weight.cpu_last)});
  if (weight.runs && weight.untimed && weight.gpu == 0.0f)
    tip.push_back({.label = "gpu: its passes are not timed"});
  else if (weight.runs)
    tip.push_back({.label = std::format("gpu {:.3f} ms a frame, {:.3f} the last frame",
                                        weight.gpu,
                                        weight.gpu_last)});
}

// A button a kind of cost after the layers', lit while the boxes show it, whose press
// shows it, or while it shows, none, as the param a save keeps and a replay restores
// (V08).
void heat_buttons(VP_VIEW::Items &items,
                  Heat heat,
                  std::string_view view,
                  std::string_view node) {
  for (std::size_t on = 0; on < heat_names.size(); ++on) {
    const bool lit = heat.on == static_cast<Heat::On>(on + 1);
    items.push_back({.label = std::string(heat_names[on]),
                     .command = lit ? std::format("{}: param unset {} heat", view, node)
                                    : std::format("{}: param set {} heat {}",
                                                  view,
                                                  node,
                                                  heat_names[on]),
                     .current = lit});
  }
}

} // namespace
