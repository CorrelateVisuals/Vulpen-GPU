#include "runtime/Operator.h"

#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace {

// When the cells step: a generation every frames_per_generation frames, counted from the
// frame index, once they started from seed-cells' cells, at the first frame and at the
// node's command; with generations set, so many after each seed, then held, so a picture
// shows the same cells whenever it is taken (C01). Engine.comp keeps the cells in two
// buffers, each generation written into the one the last was not, so the count of
// generations written says which holds them.
class Engine final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _generation = node.value<std::uint32_t>("generation");
    _stepping = node.value<std::uint32_t>("stepping");
    _seeding = node.value<std::uint32_t>("seeding");
    _frames_per_generation = node.param<std::uint32_t>("frames_per_generation");
    _generations = node.sets("generations") ? node.param<std::uint32_t>("generations") : 0;
    // Every view's commands share one port, so the command carries the node's own name.
    const std::string name = node.name();
    _seed = node.command(std::format("{} seed", name.substr(name.rfind('.') + 1)),
                         "starts the cells again from seed-cells' cells");
  }

  void cook(VP::Cook &frame) override {
    const bool seeding = std::exchange(_seed_asked, false);
    if (seeding)
      _stepped = 0;
    const bool stepping = !seeding && _frames_per_generation != 0 &&
                          frame.index() % _frames_per_generation == 0 &&
                          (_generations == 0 || _stepped < _generations);
    if (stepping)
      ++_stepped;
    frame.set(_generation, _written);
    frame.set(_seeding, std::uint32_t{seeding});
    frame.set(_stepping, std::uint32_t{stepping});
    if (seeding || stepping)
      ++_written;
  }

  void command(VP::Call &call) override {
    if (!call.is(_seed))
      return;
    _seed_asked = true;
    call.reply("the cells start again from seed-cells' cells");
  }

  VP::Value<std::uint32_t> _generation;
  VP::Value<std::uint32_t> _stepping;
  VP::Value<std::uint32_t> _seeding;
  std::uint32_t _frames_per_generation = 0;
  std::uint32_t _generations = 0; // after each seed, 0 for no end
  VP::Command _seed;
  std::uint32_t _written = 0; // generations written, the seed's too; wraps, keeping parity
  std::uint32_t _stepped = 0; // generations stepped since the last seed
  bool _seed_asked = true;    // the first frame starts the cells from the seed
};

} // namespace

VP_OPERATOR(Engine);
