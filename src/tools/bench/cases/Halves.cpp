#include "shared/Halves.h"

#include "tools/bench/Bench.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <vector>

// The library's shared/Halves.h: how a dock lays out, so the split and bar parts lay it
// out, and the docking part finds where each panel and seam shows, alike.

namespace VP::bench {

namespace domain {
// An axis, 0 for x and 1 for y, as glm indexes a vector.
struct Axis {};
// A share from 0 to 1, as the split part takes its ratio param.
struct Share {};
} // namespace domain

template <> struct Gen<domain::Axis> {
  using type = glm::length_t;
  static std::vector<glm::length_t> edges() {
    return {0, 1};
  }
  static glm::length_t random(Random &random) {
    return static_cast<glm::length_t>(random.below(2));
  }
};

template <> struct Gen<domain::Share> {
  using type = float;
  static std::vector<float> edges() {
    return {0.5f, 0.0f, 1.0f, 0.001f, 0.999f, 0.25f, 0.3333333f};
  }
  static float random(Random &random) {
    return static_cast<float>(random.below(1001)) / 1000.0f;
  }
};

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

template <> struct Shown<::Halves> {
  static std::string text(const ::Halves &split, Names &names) {
    return std::format("{} | {} | {}",
                       show(split.first, names),
                       show(split.seam, names),
                       show(split.second, names));
  }
};

void cases(Bench &bench) {
  bench.sweep("Halves.h/part_of", part_of, domain::Type{}, domain::Axis{});

  bench.sweep("Halves.h/halves", halves, domain::Type{}, domain::Axis{});
  bench.holds(
      "Halves.h/halves",
      "for a share from 0 to 1: the first, the seam and the second fill the area along "
      "the axis in turn, each whole across it",
      [](const VP_VIEW::Rect &area, glm::length_t along, float ratio) {
        constexpr std::uint32_t widest = 1U << 30; // no area is a billion pixels long
        if (area.extent[along] > widest)
          return true;
        const ::Halves split = halves(area, along, ratio);
        const glm::length_t across = 1 - along;
        const auto starts = [&](const VP_VIEW::Rect &part, std::int64_t at) {
          return part.offset[along] == at && part.offset[across] == area.offset[across] &&
                 part.extent[across] == area.extent[across];
        };
        const std::int64_t start = area.offset[along];
        return starts(split.first, start) &&
               starts(split.seam, start + split.first.extent[along]) &&
               starts(split.second,
                      start + split.first.extent[along] + split.seam.extent[along]) &&
               std::uint64_t{split.first.extent[along]} + split.seam.extent[along] +
                       split.second.extent[along] ==
                   area.extent[along];
      },
      domain::Type{},
      domain::Axis{},
      domain::Share{});

  bench.sweep("Halves.h/ratio_at", ratio_at, domain::Type{}, domain::Axis{});
  // A drag holds a seam, so the ratio it starts from is between 0 and 1, and the
  // pointer is somewhere; a seam's middle never reaches the area's ends, so a drag
  // there gives 0 or 1, which hides a half.
  bench.holds(
      "Halves.h/ratio_at",
      "a share from 0 to 1, in thousandths, that puts the seam's middle at the point",
      [](const VP_VIEW::Rect &area, glm::length_t along, float ratio, float at) {
        const std::uint32_t whole = area.extent[along];
        constexpr auto seam = static_cast<std::uint32_t>(seam_width);
        if (ratio <= 0.0f || ratio >= 1.0f || !std::isfinite(at) || whole < 2 * seam ||
            whole > (1U << 20))
          return true;
        const float made = ratio_at(area, along, ratio, at);
        const float room = static_cast<float>(whole - seam);
        const float low = static_cast<float>(area.offset[along]) + seam_width / 2.0f;
        const float high = low + room;
        const float slack = room / 1000.0f + 1.0f;
        if (made == 0.0f || made == 1.0f)
          return at <= low + slack || at >= high - slack;
        if (!(made > 0.0f && made < 1.0f) ||
            std::abs(made * 1000.0f - std::round(made * 1000.0f)) > 1e-3f)
          return false;
        const ::Halves split = halves(area, along, made);
        const float middle =
            static_cast<float>(split.seam.offset[along]) + seam_width / 2.0f;
        return std::abs(middle - std::clamp(at, low, high)) <= slack;
      },
      domain::Type{},
      domain::Axis{},
      domain::Share{},
      domain::Type{});

  bench.sweep("Halves.h/bar_length", bar_length);
  bench.holds("Halves.h/bar_length",
              "never longer than the whole, none for no rows",
              [](std::uint32_t rows, std::uint32_t cell, std::uint32_t whole) {
                const std::uint32_t length = bar_length(rows, cell, whole);
                return length <= whole && (rows != 0 || length == 0);
              });
}

} // namespace VP::bench
