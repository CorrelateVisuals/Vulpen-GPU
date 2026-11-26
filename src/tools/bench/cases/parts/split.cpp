#include "split/Split.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/split: the Rect its area gives, divided in two along its axis, the
// first taking its ratio of what the seam leaves. A press on the seam grabs it, both
// Rects follow the pointer, and letting go sends param set, so a replay puts the seam
// back. Its rects upload draws the ground and the seam, lit while grabbed or under the
// pointer.

namespace VP::bench {

namespace domain {
// The axis param as a manifest gives it.
struct AxisWord {};
// The ratio param as a manifest gives it.
struct RatioWord {};
} // namespace domain

template <> struct Gen<domain::AxisWord> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"x", "y", "", "z", "X"};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())];
  }
};

template <> struct Gen<domain::RatioWord> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"0", "0.5", "1", "0.25", "-0.1", "1.5", "nan", "x", ""};
  }
  static std::string random(Random &random) {
    return std::format("{:.3f}", static_cast<double>(random.below(1001)) / 1000.0);
  }
};

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// A frame with the input and the commands it is handed.
struct Framed {
  Running<Split> node;
  FakeInput input;
  FakeCommands commands;

  explicit Framed(Params params) : node(std::move(params)) {
    node.cook.input_port = &input;
    node.cook.command_port = &commands;
  }
  // The rects the frame drew, as the upload holds them.
  std::vector<VP_VIEW::Rect> rects() {
    const std::span<const std::byte> bytes =
        std::span(node.cook.written[0]).first(node.cook.used[0] * sizeof(VP_VIEW::Rect));
    const auto *first = reinterpret_cast<const VP_VIEW::Rect *>(bytes.data());
    return {first, first + node.cook.used[0]};
  }
};

// The two Rects after a frame with no events.
std::pair<VP_VIEW::Rect, VP_VIEW::Rect>
split(const std::string &axis, const std::string &ratio, const VP_VIEW::Rect &area) {
  Framed frame({{"axis", axis}, {"ratio", ratio}});
  frame.node.bound();
  frame.node.port<VP_VIEW::Rect>("area") = area;
  frame.node.cooked();
  return {frame.node.port<VP_VIEW::Rect>("first"),
          frame.node.port<VP_VIEW::Rect>("second")};
}

} // namespace

void cases(Bench &bench) {
  bench.check("Split.cpp/Split::bind", [](Rows &rows) {
    Running<Split> node({{"axis", "x"}, {"ratio", "0.5"}});
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });
  bench.sweep("Split.cpp/Split::cook",
              split,
              domain::AxisWord{},
              domain::RatioWord{},
              domain::Type{});

  // An area of 100 by 50 split at half: the rects drawn; the pointer over the seam lights
  // it; a press there grabs it, the Rects follow the pointer, and letting go sends param
  // set once; the pointer moved after moves nothing.
  bench.check("Split.cpp/Split::follow", [](Rows &rows) {
    Framed frame({{"axis", "x"}, {"ratio", "0.5"}});
    frame.node.bound();
    frame.node.port<VP_VIEW::Rect>("area") = {.offset = {0, 0}, .extent = {100, 50}};
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      frame.input.given = std::move(events);
      frame.commands.sent.clear();
      frame.node.cooked();
      const std::vector<VP_VIEW::Rect> drawn = frame.rects();
      rows.row(std::format("{} -> first {}, second {}, rects {}, seam {}, sent {}",
                           what,
                           show(frame.node.port<VP_VIEW::Rect>("first"), rows.names()),
                           show(frame.node.port<VP_VIEW::Rect>("second"), rows.names()),
                           show(drawn, rows.names()),
                           VP_VIEW::role_names.at(drawn.at(1).role),
                           show(frame.commands.sent, rows.names())));
    };
    const auto pointer = [](glm::vec2 at) {
      return Event{.kind = Event::Kind::pointer, .at = at};
    };
    const auto left = [](bool down) {
      return Event{.kind = Event::Kind::button, .down = down, .name = "left"};
    };
    step({}, "no events");
    const VP_VIEW::Rect seam = frame.rects().at(1);
    const glm::vec2 on_seam = glm::vec2(seam.offset) + glm::vec2(seam.extent) / 2.0f;
    step({pointer(on_seam)}, "the pointer over the seam");
    step({left(true)}, "pressed there");
    step({pointer({25, 25})}, "dragged to x 25");
    step({left(false)}, "let go");
    step({pointer({80, 25})}, "the pointer moved after");
    step({pointer({90, 25}), left(true), pointer({10, 25})},
         "pressed off the seam and moved");
  });
}

} // namespace VP::bench
