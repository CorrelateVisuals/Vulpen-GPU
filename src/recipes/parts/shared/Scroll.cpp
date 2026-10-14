// What Scroll.h declares. Only that header includes this file, at its end, so it builds
// with each part that includes the header (RV09). Each function is inline, as one a
// header defines is, so a part that uses only some of them builds without a warning.
#include "contracts/Palette.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float wheel_rows = 3.0f;   // rows a step of the wheel scrolls
constexpr float thumb_least = 16.0f; // pixels, so the thumb of many rows can be held

inline void Scroll::fit(std::size_t rows, std::size_t shown, std::size_t last) {
  _rows = rows;
  _shown = shown;
  _last = last;
  move(0.0f);
}

inline std::size_t Scroll::first() const {
  return static_cast<std::size_t>(_first);
}

inline bool Scroll::at_end() const {
  return _first >= static_cast<float>(_last);
}

inline void Scroll::wheel(float turn) {
  move(-turn * wheel_rows);
}

inline void Scroll::move(float rows) {
  _first = std::clamp(_first + rows, 0.0f, static_cast<float>(_last));
}

inline void Scroll::show(std::size_t row) {
  if (row < first())
    _first = static_cast<float>(row);
  else if (_shown > 0 && row >= first() + _shown)
    _first = static_cast<float>(row + 1 - _shown);
  move(0.0f);
}

inline bool Scroll::press(glm::vec2 at, const VP_VIEW::Rect &track) {
  const std::optional<VP_VIEW::Rect> held = thumb(track);
  if (!held || !VP_VIEW::contains(track, at))
    return false;
  _held = VP_VIEW::contains(*held, at) ? at.y - static_cast<float>(held->offset.y)
                                       : static_cast<float>(held->extent.y) / 2.0f;
  drag(at, track);
  return true;
}

inline void Scroll::drag(glm::vec2 at, const VP_VIEW::Rect &track) {
  const float room = static_cast<float>(track.extent.y) - length(track);
  if (!_held || room <= 0.0f)
    return;
  const float down = at.y - static_cast<float>(track.offset.y) - *_held;
  _first = std::clamp(
      down / room * static_cast<float>(_last), 0.0f, static_cast<float>(_last));
}

inline void Scroll::release() {
  _held.reset();
}

inline std::optional<VP_VIEW::Rect> Scroll::thumb(const VP_VIEW::Rect &track) const {
  if (_shown == 0 || (first() == 0 && _shown >= _rows))
    return std::nullopt;
  const float length = this->length(track);
  const float room = static_cast<float>(track.extent.y) - length;
  const auto down = static_cast<std::int32_t>(
      std::lround(_last == 0 ? 0.0f : room * _first / static_cast<float>(_last)));
  return VP_VIEW::Rect{
      .offset = track.offset + glm::ivec2(0, down),
      .extent = {track.extent.x, static_cast<std::uint32_t>(std::lround(length))},
      .role = VP_VIEW::role("muted")};
}

inline VP_VIEW::Rect Scroll::track(const VP_VIEW::Rect &along) {
  return {.offset = along.offset +
                    glm::ivec2(static_cast<std::int32_t>(along.extent.x) - width, 0),
          .extent = {static_cast<std::uint32_t>(width), along.extent.y}};
}

// As much of the track as the rows shown are of the rows the first may run over, and
// those it shows.
inline float Scroll::length(const VP_VIEW::Rect &track) const {
  const auto height = static_cast<float>(track.extent.y);
  const auto span = static_cast<float>(std::max(_last + _shown, std::size_t{1}));
  const float part = height * static_cast<float>(_shown) / span;
  return std::min(height, std::max(thumb_least, part));
}

} // namespace
