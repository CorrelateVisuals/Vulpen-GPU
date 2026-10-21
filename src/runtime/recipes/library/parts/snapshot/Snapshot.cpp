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
#include <string>
#include <utility>

namespace {

// Channels a pixel of the file takes: the window shows no alpha.
constexpr int rgb = 3;
constexpr int stride = 0;       // rows packed, as the pixels read back are
constexpr int channel_bits = 8; // as packUnorm4x8 packs them, red lowest

// Saves its picture as a PNG file wait frames after its file param names one, so what
// the lines sent with that param set changed has drawn by then, and a replay of them
// saves the same file (C01). A relative path names a file in its folder (RP02). The shot
// frame's shader writes the picture's pixels to a buffer, and the next frame's cook reads
// them back and saves them, once each time the param names another file; then it logs
// the file, so a script waits for it.
class Snapshot final : public VP::Operator {
  void bind(VP::Bind &node) override {
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
    if (std::exchange(_asked, false))
      _due = frame.index() + _wait;
    if (_shot && frame.index() == *_shot + 1)
      save(frame);
    const bool shoots = _due == frame.index();
    frame.set(_shoot, static_cast<std::uint32_t>(shoots));
    if (shoots) {
      _shot = frame.index();
      _size = frame.resolution();
      _due = std::nullopt;
    }
  }

  void save(VP::Cook &frame) {
    _shot = std::nullopt;
    const std::span<const std::uint32_t> pixels = frame.read(_pixels);
    const std::size_t count = std::size_t{_size.x} * _size.y;
    if (count == 0 || pixels.size() < count) {
      frame.log(VP::Level::error,
                std::format("{} not saved: a picture of {} by {} pixels, and it reads {} "
                            "at most",
                            _file,
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
    const std::string path = (std::filesystem::path(_folder) / _file).string();
    frame.files().save(path, png);
    frame.log(VP::Level::info, std::format("saved {}", path));
  }

  std::string _file; // the param's; empty for none
  std::string _folder;
  bool _asked = false; // a file named since the last cook
  std::uint32_t _wait = 0;
  std::optional<std::uint64_t> _due;  // the frame it shoots
  std::optional<std::uint64_t> _shot; // the frame its shader wrote the pixels
  glm::uvec2 _size{};                 // the picture's, that frame
  std::string _rgb;
  VP::Value<std::uint32_t> _shoot;
  VP::Readback<std::uint32_t> _pixels;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Snapshot>("Snapshot");
}
