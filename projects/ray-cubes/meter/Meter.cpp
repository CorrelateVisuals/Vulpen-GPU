#include "runtime/Operator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>

namespace {

constexpr std::size_t most_spots = 8; // as Meter.comp's invocations

// The light meter: the light gathered at spots along the picture's middle row, read back
// a frame after the GPU wrote it, said when asked, so a script checks the picture with no
// window, and any number of the chain can leave the GPU so.
class Meter final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _spots = node.readback<glm::vec4>("spots");
    // Every view's commands share one port, so the command carries the node's own name.
    const std::string name = node.name();
    _read = node.command(std::format("{} read", name.substr(name.rfind('.') + 1)),
                         "answers with the light gathered at spots along the picture's "
                         "middle row, red, green and blue each, and their mean");
  }

  void cook(VP::Cook &frame) override {
    const std::span<const glm::vec4> spots = frame.read(_spots);
    _count = std::min(spots.size(), _last.size());
    std::copy_n(spots.begin(), _count, _last.begin());
  }

  void command(VP::Call &call) override {
    if (!call.is(_read))
      return;
    if (_count == 0)
      return call.reply("none read back yet");
    std::string text;
    float sum = 0.0f;
    for (std::size_t spot = 0; spot < _count; ++spot) {
      const glm::vec4 &light = _last[spot];
      text += std::format("{:.3f},{:.3f},{:.3f} ", light.r, light.g, light.b);
      sum += (light.r + light.g + light.b) / 3.0f;
    }
    call.reply(text + std::format("mean {:.3f}", sum / static_cast<float>(_count)));
  }

  VP::Readback<glm::vec4> _spots;
  VP::Command _read;
  std::array<glm::vec4, most_spots> _last{};
  std::size_t _count = 0;
};

} // namespace

VP_OPERATOR(Meter);
