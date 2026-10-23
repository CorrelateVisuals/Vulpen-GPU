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
#include <filesystem>
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

// What a box costs, in milliseconds a frame, smoothed and as the last frame took it: the
// nodes it holds, of a view's box every node of the view and of the views it hosts, of a
// folder's every node whose files it holds, and of a file's what its code runs of its
// node; whether any of them cooks or has a pass, and whether a pass of theirs went
// untimed.
struct Weight {
  float cpu = 0.0f;
  float cpu_last = 0.0f;
  float gpu = 0.0f;
  float gpu_last = 0.0f;
  bool cooks = false;
  bool runs = false;
  bool untimed = false;
};

// Of what a node costs, what a box counts: all of it, as a box holding the node; or, as a
// file's, what its code runs, the CPU's for C++, which the operator's cook runs, and the
// GPU's for a shader, which the node's pass runs, so the boxes lead down to the code
// whichever cost shows; none for any other file, as a manifest.
enum class Part : std::uint8_t { none, all, cpu, gpu };

Part part_of(const Box &file) {
  static constexpr auto cpu =
      std::to_array({VP_VIEW::role("cpp"), VP_VIEW::role("header")});
  static constexpr auto gpu = std::to_array({VP_VIEW::role("glsl"),
                                             VP_VIEW::role("vert"),
                                             VP_VIEW::role("frag"),
                                             VP_VIEW::role("comp"),
                                             VP_VIEW::role("tesc"),
                                             VP_VIEW::role("tese"),
                                             VP_VIEW::role("geom")});
  if (!file.role)
    return Part::none;
  if (std::ranges::find(cpu, *file.role) != cpu.end())
    return Part::cpu;
  return std::ranges::find(gpu, *file.role) != gpu.end() ? Part::gpu : Part::none;
}

void add(Weight &weight, const VP::Cost &cost, Part part) {
  const bool cooks = cost.cooks && part != Part::gpu;
  const bool runs = cost.runs && part != Part::cpu;
  if (cooks) {
    weight.cpu += cost.cpu;
    weight.cpu_last += cost.cpu_last;
  }
  if (runs && cost.timed) {
    weight.gpu += cost.gpu;
    weight.gpu_last += cost.gpu_last;
  }
  weight.cooks = weight.cooks || cooks;
  weight.runs = weight.runs || runs;
  weight.untimed = weight.untimed || (runs && !cost.timed);
}

float shown(const Weight &weight, Heat heat) {
  return heat.on == Heat::On::gpu ? weight.gpu : weight.cpu;
}

// Whether a box holds a node: whose name is its own, or whose name and a dot start its
// own, as a box shows the nodes inside its node; or, joined by /, whose folder is a
// folder box's, or inside it. A level nests no box, so one box at most holds a node.
bool holds(std::string_view name, std::string_view node, char joins = '.') {
  return node == name || (node.starts_with(name) && node[name.size()] == joins);
}

// What the boxes show the cost of, as the heat param names it; what each costs this
// frame, and the most of the boxes, which the ramp's hot end stands for; and where each
// node of the views vulpen runs counts, found again only as the boxes, the view shown or
// a view's costs change, so a frame allocates nothing for it.
class Heating {
public:
  // The boxes changed, or the view they show.
  void stale() {
    _stale = true;
  }

  // While the heat shows: what each box holds costs. The names of a folder's boxes are
  // their paths from the folder holding top, the repository's; with none, the boxes are a
  // view's.
  void weigh(VP::Cook &frame,
             const Boxes &boxes,
             std::string_view view,
             std::string_view top) {
    weights.assign(boxes.boxes.size(), {});
    most = 0.0f;
    if (heat.on == Heat::On::none)
      return;
    walk(frame);
    const auto same = [](const Walked &walked, std::span<const VP::Cost> from) {
      return walked.costs.data() == from.data() && walked.costs.size() == from.size();
    };
    if (_stale || !std::ranges::equal(_walked, _from, same)) {
      _counted.clear();
      _from.clear();
      for (const Walked &walked : _walked)
        _from.push_back(walked.costs);
      if (top.empty())
        count_view(boxes, view);
      else
        count_folders(frame, boxes, top);
      _stale = false;
    }
    for (const Counted &counted : _counted)
      add(weights[counted.box], _walked[counted.view].costs[counted.cost], counted.part);
    for (const Weight &weight : weights)
      most = std::max(most, shown(weight, heat));
  }

  Heat heat;
  std::vector<Weight> weights; // by box
  float most = 0.0f;

private:
  // A view vulpen runs, by the name lines address it with, empty for the one it started
  // with; the view hosting it, among those walked; and what each of its nodes costs.
  struct Walked {
    std::string_view name;
    std::size_t host = no_box;
    std::span<const VP::Cost> costs;
  };
  // A cost, by its view among those walked and its place among the view's costs, and the
  // box it counts in, and what of it.
  struct Counted {
    std::size_t view = 0;
    std::size_t cost = 0;
    std::size_t box = 0;
    Part part = Part::all;
  };

  // The one vulpen started with first, every other after the view hosting it.
  void walk(VP::Cook &frame) {
    _walked.clear();
    _walked.push_back({});
    for (std::size_t at = 0; at < _walked.size(); ++at) {
      _walked[at].costs = frame.costs(_walked[at].name);
      if (const VP::View *const hosting = frame.view(_walked[at].name))
        for (const VP::Child &child : hosting->children)
          _walked.push_back({.name = child.name, .host = at});
    }
  }

  void counts(std::size_t view, std::size_t cost, std::size_t box, Part part) {
    if (part != Part::none)
      _counted.push_back({.view = view, .cost = cost, .box = box, .part = part});
  }

  // Of the view shown, a cost counts in the box of its node, or of the node holding it,
  // and, as the node whose inside shows, in the boxes of its files; of a view it hosts,
  // or one hosted by way of those, in that view's box, so a view's box holds all it runs.
  void count_view(const Boxes &boxes, std::string_view view) {
    for (std::size_t walked = 0; walked < _walked.size(); ++walked) {
      const std::span<const VP::Cost> costs = _walked[walked].costs;
      std::size_t hosted = walked;
      while (_walked[hosted].host != no_box && _walked[_walked[hosted].host].name != view)
        hosted = _walked[hosted].host;
      const std::size_t box = _walked[hosted].host == no_box
                                  ? no_box
                                  : view_box(boxes, _walked[hosted].name);
      for (std::size_t cost = 0; cost < costs.size(); ++cost)
        if (_walked[walked].name != view)
          counts(walked, cost, box, box < boxes.boxes.size() ? Part::all : Part::none);
        else
          for (std::size_t at = 0; at < boxes.boxes.size(); ++at) {
            const Box &holder = boxes.boxes[at];
            const std::string_view node = costs[cost].node;
            counts(walked,
                   cost,
                   at,
                   !holder.view && holds(holder.name, node)        ? Part::all
                   : node == boxes.scope && !holder.opens.empty() ? part_of(holder)
                                                                  : Part::none);
          }
    }
  }

  // Of a folder's boxes, every view's costs, so a folder holds what all the code in it
  // costs: a node's counts in the box of the folder holding its folder, as a node's
  // folder holds its files, and in the boxes of those files.
  void count_folders(VP::Cook &frame, const Boxes &boxes, std::string_view top) {
    std::vector<std::string> places; // by box, a folder's or the one holding a file
    for (const Box &box : boxes.boxes) {
      const std::filesystem::path folder =
          box.opens.empty() ? folder_path(top, box.name) : box.opens.parent_path();
      places.push_back(box.role ? folder.lexically_normal().generic_string() : "");
    }
    for (std::size_t walked = 0; walked < _walked.size(); ++walked) {
      const std::span<const VP::Cost> costs = _walked[walked].costs;
      const VP::View *const flat = frame.unfolded(_walked[walked].name);
      for (std::size_t cost = 0; flat && cost < costs.size(); ++cost) {
        const auto node =
            std::ranges::find(flat->nodes, costs[cost].node, &VP::Node::name);
        if (node == flat->nodes.end())
          continue;
        const std::string code = node->folder.lexically_normal().generic_string();
        for (std::size_t box = 0; box < boxes.boxes.size(); ++box)
          if (const std::string &place = places[box]; !place.empty())
            counts(walked,
                   cost,
                   box,
                   !boxes.boxes[box].opens.empty()
                       ? (code == place ? part_of(boxes.boxes[box]) : Part::none)
                       : (holds(place, code, '/') ? Part::all : Part::none));
      }
    }
  }

  std::vector<Walked> _walked; // this frame's
  std::vector<std::span<const VP::Cost>> _from; // the costs counted were found in
  std::vector<Counted> _counted;
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

// After a divider, a button a kind of cost, the two a switch, as edit and run, lit while
// the boxes show it, whose press shows it, or while it shows, none, as the param a save
// keeps and a replay restores (V08).
void heat_buttons(VP_VIEW::Items &items,
                  Heat heat,
                  std::string_view view,
                  std::string_view node) {
  items.push_back({});
  for (std::size_t on = 0; on < heat_names.size(); ++on) {
    const bool lit = heat.on == static_cast<Heat::On>(on + 1);
    items.push_back({.label = std::string(heat_names[on]),
                     .command = lit ? std::format("{}: param unset {} heat", view, node)
                                    : std::format("{}: param set {} heat {}",
                                                  view,
                                                  node,
                                                  heat_names[on]),
                     .current = lit,
                     .toggles = true});
  }
}

} // namespace
