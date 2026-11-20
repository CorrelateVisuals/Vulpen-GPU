#include "runtime/Operator.h"
#include "shared/Wave.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Records what each frame played, read a frame after, as the port hands it on, until it
// holds seconds of it, then saves it to the file its file param names, once each time
// the param names another (RA04: a write the run was asked for). Its memory is bounded by
// seconds (A03).
class SoundRecord final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string file = node.sets("file") ? node.param<std::string>("file") : "";
    // A rebuild binds again, naming the same file, which it saved already.
    if (std::exchange(_file, file) != file) {
      _recorded.clear();
      _recording = !_file.empty();
      _fresh = true;
    }
    _folder = node.folder();
    const float seconds = node.param<float>("seconds");
    if (!(seconds > 0.0f))
      throw std::runtime_error(std::format("seconds = {} records nothing", seconds));
    _wanted = static_cast<std::size_t>(std::lround(seconds * VP::SoundPort::sample_rate)) *
              VP::SoundPort::channels;
  }

  void cook(VP::Cook &frame) override {
    if (!_recording)
      return;
    // What played the frame before is the frame before the file was named.
    if (std::exchange(_fresh, false))
      return;
    const std::span<const float> played = frame.sound().played();
    const std::size_t taken = std::min(played.size(), _wanted - _recorded.size());
    std::copy_n(played.begin(), taken, std::back_inserter(_recorded));
    if (_recorded.size() < _wanted)
      return;
    _recording = false;
    const std::string path = (std::filesystem::path(_folder) / _file).string();
    frame.files().save(path, wave_write(_recorded));
    frame.log(VP::Level::info, std::format("saved {}", path));
  }

  std::string _file; // the param's; empty for none
  std::string _folder;
  std::size_t _wanted = 0; // samples, two a sample frame
  std::vector<float> _recorded;
  bool _recording = false;
  bool _fresh = false; // the file was named since the frame before
};

} // namespace

VP_OPERATOR(SoundRecord);
