#include "runtime/Operator.h"
#include "shared/Picture.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// "For he whom God hath sent speaketh the words of God: for God giveth not the Spirit by
// measure unto him." John 3:34

constexpr std::string_view all_levels = "all";

// The mip levels a param names: a count, or every one.
std::uint32_t mip_levels(std::string_view text) {
  if (text == all_levels)
    return VP::all_mip_levels;
  std::uint32_t count = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), count);
  if (error != std::errc{} || end != text.data() + text.size() || count == 0)
    throw std::runtime_error(
        std::format("mip_levels = {} is no count of levels, nor {}", text, all_levels));
  return count;
}

// A picture file as a frame tells it changed: its size and the hashes of its first and
// last end_bytes. Hashing all of a picture of megabytes each frame costs (C03), and a
// PNG ends in a checksum of all its pixels, so a repaint anywhere in one changes its end.
constexpr std::size_t end_bytes = 64 * 1024;
struct Fingerprint {
  std::size_t size = 0;
  std::size_t head = 0;
  std::size_t tail = 0;
  bool operator==(const Fingerprint &) const = default;
};

Fingerprint fingerprint(std::string_view bytes) {
  const std::hash<std::string_view> hash;
  const std::size_t tail_from = bytes.size() > end_bytes ? bytes.size() - end_bytes : 0;
  return {.size = bytes.size(),
          .head = hash(bytes.substr(0, end_bytes)),
          .tail = hash(bytes.substr(tail_from))};
}

// The picture file its file param names, decoded into its picture, sRGB as a file holds
// its colors: read again once it changes on disk, so art saved elsewhere shows at once.
// A file it cannot read or decode is named in the log, and the picture keeps what it
// held.
class Picture final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _file = node.sets("file") ? node.param<std::string>("file") : "";
    _mip_levels = mip_levels(node.param<std::string>("mip_levels"));
    _picture = node.texture<glm::u8vec4>("picture");
    _watched = _file.empty() ? VP::File{} : node.file(_file);
  }

  void cook(VP::Cook &frame) override {
    if (_file.empty())
      return;
    const std::string_view bytes = frame.files().text(_watched);
    const Fingerprint read = fingerprint(bytes);
    if (_decoded == read)
      return;
    _decoded = read; // decoded once, whether it decodes or not, until it changes
    try {
      const Pixels pixels = decoded(bytes, _file);
      frame.upload(_picture, pixels.rgba, pixels.size, _mip_levels);
    } catch (const std::runtime_error &failure) {
      frame.log(VP::Level::error, failure.what());
    }
  }

  std::string _file; // the param's, as an absolute path; empty for none
  std::uint32_t _mip_levels = 1;
  VP::Texture<glm::u8vec4> _picture;
  VP::File _watched;
  // The file the picture holds, by its fingerprint; none until one decoded.
  std::optional<Fingerprint> _decoded;
};

} // namespace

VP_OPERATOR(Picture);
