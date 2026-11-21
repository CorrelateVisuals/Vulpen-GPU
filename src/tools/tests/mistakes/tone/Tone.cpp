#include "runtime/Operator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint64_t period = 128; // samples a saw rises over, each a whole step
constexpr float half_period = 64.0f;
constexpr std::uint64_t loud_every = 50; // a sample past 1, which the port clamps
constexpr float loud = 3.0f;
constexpr std::uint64_t frames_a_second = 60; // the run's, at --fps 0

// Adds a saw to the frame's sound, every sample exact in a float, with a sample past 1
// now and then, and stops when what played the frame before is other than it added,
// clamped, or when a frame takes another count of samples than the run's rate gives.
class Tone final : public VP::Operator {
  void cook(VP::Cook &frame) override {
    VP::SoundPort &sound = frame.sound();
    const std::span<const float> played = sound.played();
    if (!std::ranges::equal(played, _expected))
      throw std::runtime_error(
          std::format("frame {} heard {} samples other than the {} it added",
                      frame.index(),
                      played.size(),
                      _expected.size()));
    if (sound.frames() != VP::SoundPort::sample_rate / frames_a_second)
      throw std::runtime_error(
          std::format("frame {} takes {} sample frames", frame.index(), sound.frames()));
    std::vector<float> added(sound.frames() * VP::SoundPort::channels);
    for (float &sample : added) {
      sample = _next % loud_every == 0
                   ? loud
                   : static_cast<float>(_next % period) / half_period - 1.0f;
      ++_next;
    }
    sound.add(added);
    _expected = added;
    for (float &sample : _expected)
      sample = std::clamp(sample, -1.0f, 1.0f);
  }

  std::uint64_t _next = 0;
  std::vector<float> _expected;
};

} // namespace

VP_OPERATOR(Tone);
