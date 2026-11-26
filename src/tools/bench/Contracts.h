#pragma once

#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Rect.h"
#include "tools/bench/Domains.h"
#include "tools/bench/Shown.h"

#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// The library's contracts (RV05) as rows make and show them: what a part's connections
// carry, and what shared code takes, for the cases of both, which build with the
// library's includes (bench.cmake).

namespace VP::bench {

template <> struct Gen<VP_VIEW::Rect> {
  using type = VP_VIEW::Rect;
  static std::vector<VP_VIEW::Rect> edges() {
    constexpr std::uint32_t most = std::numeric_limits<std::uint32_t>::max();
    return {{.offset = {10, 20}, .extent = {200, 100}},
            {},
            {.offset = {0, 0}, .extent = {1, 1}},
            {.offset = {0, 0}, .extent = {3, 3}},
            {.offset = {0, 0}, .extent = {4, 4}},
            {.offset = {0, 0}, .extent = {5, 5}},
            {.offset = {-50, -50}, .extent = {100, 100}},
            {.offset = {0, 0}, .extent = {most, most}}};
  }
  static VP_VIEW::Rect random(Random &random) {
    return {.offset = {static_cast<std::int32_t>(random.below(2001)) - 1000,
                       static_cast<std::int32_t>(random.below(2001)) - 1000},
            .extent = {static_cast<std::uint32_t>(random.below(4000)),
                       static_cast<std::uint32_t>(random.below(4000))}};
  }
};

template <> struct Shown<VP_VIEW::Rect> {
  static std::string text(const VP_VIEW::Rect &rect, Names &names) {
    return std::format("{}+{}", show(rect.offset, names), show(rect.extent, names));
  }
};

// An Item by what a list shows and sends of it, its label and command; one with neither
// is a divider.
template <> struct Gen<VP_VIEW::Item> {
  using type = VP_VIEW::Item;
  static std::vector<VP_VIEW::Item> edges() {
    return {{.label = "open", .command = "open"}, {}, {.label = "save"}};
  }
  static VP_VIEW::Item random(Random &random) {
    constexpr std::array<std::string_view, 4> labels{"a", "b", "c", ""};
    return {.label = std::string(labels[random.below(labels.size())]),
            .command = random.below(2) == 0 ? "" : "x"};
  }
};

template <> struct Shown<VP_VIEW::Item> {
  static std::string text(const VP_VIEW::Item &item, Names &) {
    if (item.label.empty() && item.command.empty())
      return "|";
    return item.command.empty() ? item.label
                                : std::format("{}>{}", item.label, item.command);
  }
};

// A font by its cell, the size a part lays text out in.
template <> struct Gen<VP_VIEW::Metrics> {
  using type = VP_VIEW::Metrics;
  static std::vector<VP_VIEW::Metrics> edges() {
    return {{.font = {.cell = {8, 16}}},
            {},
            {.font = {.cell = {1, 1}}},
            {.font = {.cell = {7, 13}}}};
  }
  static VP_VIEW::Metrics random(Random &random) {
    return {.font = {.cell = {static_cast<std::uint32_t>(random.below(40)),
                              static_cast<std::uint32_t>(random.below(40))}}};
  }
};

template <> struct Shown<VP_VIEW::Metrics> {
  static std::string text(const VP_VIEW::Metrics &metrics, Names &names) {
    return "cell " + show(metrics.font.cell, names);
  }
};

} // namespace VP::bench
