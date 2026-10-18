#pragma once

#include "contracts/Corner.h"
#include "runtime/Operator.h"

#include <cstdint>

// Only Corners.cpp includes this header, so the class stays in the unnamed namespace, and
// two copies of the node in one binary never clash.
namespace {

// Turns the classic triangle's corners every frame and hands them, each with its color,
// to the face, which draws them.
class Corners final : public VP::Operator {
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;

  VP::Upload<VP_VIEW::Corner> _corners;
  std::uint32_t _frames_per_turn = 0;
};

} // namespace
