#include "runtime/Operator.h"
#include "shared/Wave.h"

#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The WAV file its file param names, added to the frame's sound a frame's samples at a
// time, from where the frame before left off, so a replay plays the same samples (C01):
// from its start once the file is named or changes on disk, over and over while loop is
// yes, scaled by volume. A file it cannot read is named in the log, and it plays nothing.
class SoundFile final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string file = node.sets("file") ? node.param<std::string>("file") : "";
    if (std::exchange(_file, file) != file)
      _decoded.reset();
    const std::string loop = node.param<std::string>("loop");
    if (loop != "yes" && loop != "no")
      throw std::runtime_error(std::format("loop = {} is not yes or no", loop));
    _loop = loop == "yes";
    _volume = node.param<float>("volume");
    _watched = _file.empty() ? VP::File{} : node.file(_file);
  }

  void cook(VP::Cook &frame) override {
    if (_file.empty())
      return;
    const std::string_view bytes = frame.files().text(_watched);
    const std::size_t read = std::hash<std::string_view>{}(bytes);
    if (_decoded != read) {
      _decoded = read; // decoded once, whether it decodes or not, until it changes
      _at = 0;
      try {
        _samples = wave_read(bytes, _file);
      } catch (const std::runtime_error &failure) {
        _samples.clear();
        frame.log(VP::Level::error, failure.what());
      }
    }
    VP::SoundPort &sound = frame.sound();
    _frame.assign(sound.frames() * VP::SoundPort::channels, 0.0f);
    for (float &sample : _frame) {
      if (_at == _samples.size() && _loop)
        _at = 0;
      if (_at == _samples.size())
        break;
      sample = _samples[_at++] * _volume;
    }
    sound.add(_frame);
  }

  std::string _file; // the param's, as an absolute path; empty for none
  bool _loop = false;
  float _volume = 1.0f;
  VP::File _watched;
  // The file's bytes the samples are of, by their hash; none until one decoded.
  std::optional<std::size_t> _decoded;
  std::vector<float> _samples; // two channels, interleaved
  std::size_t _at = 0;         // the next sample to play
  std::vector<float> _frame;   // kept, so a frame allocates nothing once grown (CPP10)
};

} // namespace

VP_OPERATOR(SoundFile);
