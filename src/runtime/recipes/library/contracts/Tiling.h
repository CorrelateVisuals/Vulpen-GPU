#pragma once

#include "runtime/Operator.h"

#include <string>
#include <string_view>

namespace VP_VIEW {

// A content that divides its own room, showing several of what it holds side by side, as
// the tiles part shows projects in the Perform panel: the docking part asks it, as the
// strip of the panel holding it is let go on that panel's own edge or middle, rather than
// splitting the panel, so the panel stays one.
class Tiling {
public:
  // Its node, as the panel holding it reaches it.
  virtual const std::string &name() const = 0;
  // Whether it holds another to show beside the one selected, and whether it shows
  // several now, which a drop in its middle makes one again.
  virtual bool more() const = 0;
  virtual bool several() const = 0;
  // The one selected goes beside the others, at the side named, left, right, top or
  // bottom, or with middle shows alone again.
  virtual void divide(VP::Call &call, std::string_view side) = 0;

protected:
  ~Tiling() = default;
};

// What a tiles part hands the docking part: itself.
struct Tiled {
  Tiling *tiling = nullptr;
};

} // namespace VP_VIEW
