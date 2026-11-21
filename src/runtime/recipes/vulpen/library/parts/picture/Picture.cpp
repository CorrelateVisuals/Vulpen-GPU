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
    const std::size_t read = std::hash<std::string_view>{}(bytes);
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
  // The file's bytes the picture holds, by their hash; none until one decoded.
  std::optional<std::size_t> _decoded;
};

} // namespace

VP_OPERATOR(Picture);
