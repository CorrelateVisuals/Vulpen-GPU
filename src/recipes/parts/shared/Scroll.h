#pragma once

#include "contracts/Rect.h"
#include "runtime/Operator.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace {

// Rows of which as many as fit show at once, from a first row that the wheel and a drag
// on a bar's thumb move, as the editor's and the terminal's. The first row keeps its
// fraction, so a touchpad's many small turns add up, where each alone would round to no
// row. The part hands it the turns, presses and pointer moves it takes, so the part still
// decides what a press is for (RV09).
class Scroll {
public:
  // How wide a bar is, down the right edge of what scrolls, which a part keeps its text
  // out of.
  static constexpr std::int32_t width = 10;

  // The rows in all, as many as show at once, and the last row that may show first: the
  // one that leaves the last row at the bottom, or a later one, as an editor scrolls on
  // until its last row shows alone. Before the rest, whenever one of them changed.
  void fit(std::size_t rows, std::size_t shown, std::size_t last);
  std::size_t first() const;
  // Whether no later row may show first, as a terminal's newest rows show.
  bool at_end() const;
  // Up, a positive turn, shows the rows above.
  void wheel(float turn);
  void move(float rows);
  // As far as it must, so the row shows.
  void show(std::size_t row);
  // A press on the bar, down the track: it holds the thumb, once the thumb's middle came
  // to the press when the press missed it. Whether it took the press, which the part
  // then takes no further.
  bool press(glm::vec2 at, const VP_VIEW::Rect &track);
  // While the thumb is held, the pointer moves it, and the rows with it.
  void drag(glm::vec2 at, const VP_VIEW::Rect &track);
  void release();
  // The thumb on the track while a row does not show; none while all do.
  std::optional<VP_VIEW::Rect> thumb(const VP_VIEW::Rect &track) const;
  // The track down the right edge of what scrolls.
  static VP_VIEW::Rect track(const VP_VIEW::Rect &along);

private:
  float length(const VP_VIEW::Rect &track) const;

  float _first = 0.0f;
  std::size_t _rows = 0;
  std::size_t _shown = 0;
  std::size_t _last = 0;
  std::optional<float> _held; // where the pointer holds the thumb, from its top
};

} // namespace

#include "Scroll.cpp"
