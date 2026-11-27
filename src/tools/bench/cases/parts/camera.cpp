#include "camera/Camera.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/camera: a camera turned and panned about a target with the pointer,
// a left drag turning the scene as a ball or level, a right or middle drag moving the
// target, the wheel bringing the eye nearer or farther, a double left press home, where
// its params put it; turning about up by itself every frames_per_turn frames. It writes
// one Camera a frame: the eye, the frame it looks along, the lens, near and far.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// Its params as its view.vlp drops it, those given set otherwise.
Params dropped(const Params &changed = {}) {
  Params params{{"target", "0,0,0"},
                {"eye", "0,0,-2"},
                {"up", "0,-1,0"},
                {"lens", "2"},
                {"turn", "ball"},
                {"frames_per_turn", "0"},
                {"near", "0.01"},
                {"far", "1000"},
                {"double_click", "home"}};
  for (const auto &[key, value] : changed)
    for (auto &param : params)
      if (param.first == key)
        param.second = value;
  return params;
}

// A number to 4 places, as a turn leaves its last bits to the library's sine and cosine.
std::string rounded(float value) {
  constexpr float places = 1e4f;
  const float kept = std::round(value * places) / places;
  return number_text(kept == 0.0f ? 0.0f : kept);
}

std::string xyz(const glm::vec4 &vector) {
  return std::format(
      "({} {} {})", rounded(vector.x), rounded(vector.y), rounded(vector.z));
}

std::string camera_text(const VP_VIEW::Camera &camera) {
  return std::format("eye {} ahead {} right {} up {}; lens {} near {} far {}",
                     xyz(camera.eye),
                     xyz(camera.ahead),
                     xyz(camera.right),
                     xyz(camera.up),
                     rounded(camera.ahead.w),
                     rounded(camera.right.w),
                     rounded(camera.up.w));
}

Event pointer(glm::vec2 at) {
  return {.kind = Event::Kind::pointer, .at = at};
}
Event button(std::string name, bool down, std::uint32_t presses = 1) {
  return {.kind = Event::Kind::button,
          .down = down,
          .name = std::move(name),
          .presses = down ? presses : 0};
}
Event wheel(float steps) {
  return {.kind = Event::Kind::wheel, .turn = {0.0f, steps}};
}
// A press at from, the pointer moved to to, let go: a drag of that button.
std::vector<Event> drag(std::string name, glm::vec2 from, glm::vec2 to) {
  return {pointer(from), button(name, true), pointer(to), button(std::move(name), false)};
}

// The node, its picture 200 by 100, so a drag across 100 pixels turns half a turn.
struct Framed {
  Running<Camera> node;
  FakeInput input;

  explicit Framed(Params params) : node(std::move(params)) {
    node.bound();
    node.cook.input_port = &input;
    node.cook.size = {200, 100};
  }
  // The Camera a frame of these events writes, in the room its last bind asked for.
  std::string cooked(std::vector<Event> events, std::uint64_t frame = 0) {
    input.given = std::move(events);
    node.cook.frame = frame;
    node.cooked();
    return camera_text(
        *reinterpret_cast<const VP_VIEW::Camera *>(node.cook.written.back().data()));
  }
};

} // namespace

void cases(Bench &bench) {
  // Its params as dropped, and each it refuses: a lens of 0 or less, a word it does not
  // take, near not beyond 0 or far not beyond near, and eye, target and up that leave no
  // frame.
  bench.check("Camera.cpp/Camera::bind", [](Rows &rows) {
    const std::vector<std::pair<std::string_view, Params>> cases{
        {"as dropped", {}},
        {"lens 0", {{"lens", "0"}}},
        {"lens -1", {{"lens", "-1"}}},
        {"lens nan", {{"lens", "nan"}}},
        {"turn x", {{"turn", "x"}}},
        {"double_click x", {{"double_click", "x"}}},
        {"near 0", {{"near", "0"}}},
        {"far below near", {{"far", "0.001"}}},
        {"far at near", {{"far", "0.01"}}},
        {"eye on the target", {{"eye", "0,0,0"}}},
        {"up along the line of sight", {{"up", "0,0,1"}}}};
    for (const auto &[what, changed] : cases) {
      Running<Camera> node(dropped(changed));
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", what, said));
    }
  });

  // Home; a left drag across turns the scene as a ball under the pointer rolls, half a
  // turn for the picture's shorter side; a right drag moves the target, the scene
  // following the pointer; the wheel; a double left press home again.
  bench.check("Camera.cpp/Camera::cook", [](Rows &rows) {
    Framed frame(dropped());
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events))));
    };
    step({}, "home");
    step(drag("left", {100, 50}, {150, 50}), "a left drag 50 pixels right");
    step({pointer({100, 50}), button("left", true, 2)}, "a double left press");
    step(drag("left", {100, 50}, {100, 75}), "a left drag 25 pixels down");
    step({pointer({100, 50}), button("left", true, 2), button("left", false)},
         "home again, after the turn");
    step(drag("right", {100, 50}, {150, 50}), "a right drag 50 pixels right");
    step(drag("middle", {150, 50}, {150, 0}), "a middle drag 50 pixels up");
    step({pointer({100, 50}), button("left", true, 2), button("left", false)},
         "home again, after the pan");
    step({wheel(1)}, "the wheel a step in");
    step({wheel(-2)}, "the wheel two steps out");
    step({wheel(100)}, "the wheel 100 steps in");
    step({wheel(-200)}, "the wheel 200 steps out");
    step({pointer({120, 60})}, "the pointer moved, no button down");
    step(drag("left", {120, 60}, {120, 60}), "a left drag that moves nowhere");
  });

  // Level turning: a drag across turns about up, a drag down tilts the eye up over the
  // target, never past straight down; with double_click none a double press turns too.
  bench.check("Camera.cpp/Camera::level", [](Rows &rows) {
    Framed frame(dropped({{"turn", "level"}, {"double_click", "none"}}));
    const auto step = [&](std::vector<Event> events, std::string_view what) {
      rows.row(std::format("{} -> {}", what, frame.cooked(std::move(events))));
    };
    step(drag("left", {100, 50}, {150, 50}), "a left drag 50 pixels right");
    step(drag("left", {100, 50}, {100, 75}), "a left drag 25 pixels down");
    step(drag("left", {100, 50}, {100, 250}), "a left drag 200 pixels down");
    step(drag("left", {100, 50}, {100, -350}), "a left drag 400 pixels up");
    step({pointer({100, 50}),
          button("left", true, 2),
          pointer({150, 50}),
          button("left", false)},
         "a double left press dragged 50 pixels right");
  });

  // A whole turn about up every frames_per_turn frames, counted from the frame index;
  // none for 0.
  bench.check("Camera.cpp/Camera::spin_at", [](Rows &rows) {
    for (const std::string_view frames_per_turn : {"0", "4"}) {
      Framed frame(dropped({{"frames_per_turn", std::string(frames_per_turn)}}));
      for (const std::uint64_t index : {0u, 1u, 2u, 5u})
        rows.row(std::format("frames_per_turn {}, frame {} -> {}",
                             frames_per_turn,
                             index,
                             frame.cooked({}, index)));
    }
  });

  // Bound again, as the engine binds every node of a view whose schedule it builds anew:
  // after any edit of the view, and as its picture moves between the window and a pane.
  bench.check("Camera.cpp/Camera::bind, bound again", [](Rows &rows) {
    for (const auto &[key, value] :
         Params{{"eye", "0,0,-2"}, {"lens", "3"}, {"eye", "0,0,-4"}}) {
      Framed frame(dropped());
      std::string turned = frame.cooked(drag("left", {100, 50}, {150, 50}));
      turned += "; " + frame.cooked({wheel(1)});
      frame.node.bind.params = dropped({{key, value}});
      frame.node.bound();
      rows.row(std::format("turned and nearer: {}; bound again, {} {}: {}",
                           turned,
                           key,
                           value,
                           frame.cooked({})));
    }
  });
}

} // namespace VP::bench
