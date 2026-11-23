#include "runtime/Operator.h"

// Static, so each view's copy of this part keeps stb_image_write to itself, and copies in
// two views link into one release binary; it writes into memory, which the file port
// saves, so it holds no code that opens a file.
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// "Who is the image of the invisible God, the firstborn of every creature:"
// Colossians 1:15

// Channels a pixel of the file takes: the window shows no alpha.
constexpr int rgb = 3;
constexpr int stride = 0;       // rows packed, as the pixels read back are
constexpr int channel_bits = 8; // as packUnorm4x8 packs them, red lowest
// The most pictures one recording takes: a minute at 60 frames a second. A 1080p picture
// of a busy scene takes a few MB, so a slipped digit can't fill the disk.
constexpr std::uint32_t most_frames = 3600;

// A recording's picture: its number before the ending, as wide as the count, so the
// names sort in frame order, as ffmpeg's -i shot-%03d.png reads them.
std::string numbered(const std::string &file, std::uint32_t number, std::uint32_t count) {
  const std::filesystem::path path(file);
  const std::size_t width = std::to_string(count).size();
  return (path.parent_path() / std::format("{}-{:0{}}{}",
                                           path.stem().string(),
                                           number,
                                           width,
                                           path.extension().string()))
      .string();
}

// Saves its picture as a PNG file wait frames after its file param names one, so what
// the lines sent with that param set changed has drawn by then, and a replay of them
// saves the same file (C01). A relative path names a file in its folder (RP02). The shot
// frame's shader writes the picture's pixels to a buffer, and the next frame's cook reads
// them back and saves them, once each time the param names another file; then it logs
// the file, so a script waits for it. With frames above 1 it records: it shoots that many
// frames in a row. A frame's time counts frames, not the clock, so however slowly the
// pictures save, they play back smoothly at the run's rate.
class Snapshot final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::uint32_t frames = node.param<std::uint32_t>("frames");
    if (frames == 0 || frames > most_frames)
      throw std::runtime_error(
          std::format("param frames = {}: from 1 to {}", frames, most_frames));
    _frames = frames;
    const std::string file = node.sets("file") ? node.param<std::string>("file") : "";
    // A rebuild binds again, naming the same file, which it saved already.
    if (std::exchange(_file, file) != file) {
      _asked = !_file.empty();
      _due = std::nullopt;
    }
    _folder = node.folder();
    _wait = node.param<std::uint32_t>("wait");
    _shoot = node.value<std::uint32_t>("shoot");
    _pixels = node.readback<std::uint32_t>("pixels");
  }

  void cook(VP::Cook &frame) override {
    if (std::exchange(_asked, false)) {
      _due = frame.index() + _wait;
      _taken = 0;
    }
    if (_shot && frame.index() == *_shot + 1)
      save(frame);
    const bool shoots = _due == frame.index();
    frame.set(_shoot, static_cast<std::uint32_t>(shoots));
    if (shoots)
      shoot(frame);
  }

  // The path is named as it shoots, so a file param set before the save names another
  // file only for the pictures after it.
  void shoot(VP::Cook &frame) {
    ++_taken;
    _shot = frame.index();
    _size = frame.resolution();
    const std::string name = _frames == 1 ? _file : numbered(_file, _taken, _frames);
    _path = (std::filesystem::path(_folder) / name).string();
    // At least, as a param set may lower frames while it records.
    _ends = _taken >= _frames;
    _due = _ends ? std::nullopt : std::optional(frame.index() + 1);
  }

  void save(VP::Cook &frame) {
    _shot = std::nullopt;
    const std::span<const std::uint32_t> pixels = frame.read(_pixels);
    const std::size_t count = std::size_t{_size.x} * _size.y;
    if (count == 0 || pixels.size() < count) {
      frame.log(VP::Level::error,
                std::format("{} not saved: a picture of {} by {} pixels, and it reads {} "
                            "at most",
                            _path,
                            _size.x,
                            _size.y,
                            pixels.size()));
      return;
    }
    _rgb.resize(count * rgb);
    for (std::size_t at = 0; at < count; ++at)
      for (std::size_t channel = 0; channel < rgb; ++channel)
        _rgb[at * rgb + channel] =
            static_cast<char>(pixels[at] >> (channel * channel_bits));
    std::string png;
    stbi_write_png_to_func(
        [](void *into, void *data, int size) {
          static_cast<std::string *>(into)->append(static_cast<const char *>(data),
                                                   static_cast<std::size_t>(size));
        },
        &png,
        static_cast<int>(_size.x),
        static_cast<int>(_size.y),
        rgb,
        _rgb.data(),
        stride);
    frame.files().save(_path, png);
    // A recording's pictures but its last log at debug, so a person or a script reading
    // at info sees one line for it (C09).
    frame.log(_ends ? VP::Level::info : VP::Level::debug, std::format("saved {}", _path));
  }

  std::string _file; // the param's; empty for none
  std::string _folder;
  bool _asked = false; // a file named since the last cook
  std::uint32_t _wait = 0;
  std::uint32_t _frames = 1;
  std::uint32_t _taken = 0;           // pictures shot since the file was named
  std::optional<std::uint64_t> _due;  // the frame it shoots
  std::optional<std::uint64_t> _shot; // the frame its shader wrote the pixels
  glm::uvec2 _size{};                 // the picture's, that frame
  std::string _path;                  // the picture's file
  bool _ends = false;                 // the last of its recording, or a lone picture
  std::string _rgb;
  VP::Value<std::uint32_t> _shoot;
  VP::Readback<std::uint32_t> _pixels;
};

} // namespace

VP_OPERATOR(Snapshot);
