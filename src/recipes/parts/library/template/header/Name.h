#pragma once

#include "runtime/Operator.h"

// Only Name.cpp includes this header: a recipe's C++ is one translation unit, so the
// class stays in the unnamed namespace and two views' copies of the recipe never clash.
namespace {

// Why the Name operator exists, in a line or two.
class Name final : public VP::Operator {
  // VP::Value<float> _scale;       // a value in the pass block
  // VP::Upload<glm::vec2> _points; // a buffer C++ writes and the shader reads
  // VP::Readback<float> _samples;  // a buffer the shader writes, read a frame later
  // float _speed = 0;              // a param
  // VP::Texture _atlas;            // an image C++ fills once
  // VP::Command _reset;            // a command the node answers
  // VP::File _settings;            // a file the node reads, watches and saves
  // std::string _folder;           // where the recipe's own files are

  // At load, and again after every swap: names in, handles out.
  // void bind(VP::Bind &node) override;

  // Every frame, before the node's pass runs.
  // void cook(VP::Cook &frame) override;

  // When a command the node registered runs.
  // void command(VP::Call &call) override;
};

} // namespace
