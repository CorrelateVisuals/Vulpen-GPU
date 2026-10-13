#include "contracts/Font.h"
#include "runtime/Operator.h"

// Static, so each view's copy of this part keeps stb_truetype to itself, and copies in
// two views link into one release binary.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Printable ASCII, from the space to the tilde.
constexpr std::uint32_t first = ' ';
constexpr std::uint32_t count = '~' - first + 1;
constexpr std::uint32_t columns = 16; // of glyphs in a row of the atlas
constexpr std::uint32_t rows = (count + columns - 1) / columns;
constexpr float zoom_step = 1.1f; // how much larger a step of zoom in draws
constexpr float zoom_least = 0.5f;
constexpr float zoom_most = 4.0f;
constexpr std::string_view kept_file = ".vulpen/font.ini"; // which no build or scan reads
constexpr std::string_view blanks = " \t\r";
constexpr std::string_view kept_header =
    "# How much larger than its height param the font draws the text, which a restart\n"
    "# keeps. zoom in, zoom out and zoom reset write it; delete it to draw the text at\n"
    "# the font's height again.\n";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// The line zoom = z, as a zoom writes it; # starts a comment, and with none the font
// draws at its height. Throws naming the first line that is no such zoom (A02).
float zoom_of(std::string_view text) {
  float zoom = 1.0f;
  for (std::size_t line = 1; !text.empty(); ++line) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    const std::string_view row = trim(text.substr(0, std::min(text.find('#'), end)));
    text.remove_prefix(std::min(end + 1, text.size()));
    if (row.empty())
      continue;
    const std::size_t equals = std::min(row.find('='), row.size());
    const std::string_view value = trim(row.substr(std::min(equals + 1, row.size())));
    const auto [stop, error] =
        std::from_chars(value.data(), value.data() + value.size(), zoom);
    if (trim(row.substr(0, equals)) != "zoom" || error != std::errc{} ||
        stop != value.data() + value.size() || !(zoom >= zoom_least && zoom <= zoom_most))
      throw std::runtime_error(std::format("{}:{}: {} is no zoom = z from {} to {}",
                                           kept_file,
                                           line,
                                           row,
                                           zoom_least,
                                           zoom_most));
  }
  return zoom;
}

// Bakes the monospace font file its face param names at the height its height param
// gives, times its zoom, a cell a glyph, into an atlas that glyphs samples a texel a
// pixel. It bakes after it binds, as an edit of either param makes it do, once the
// zoom changes the height, and once image clear empties the atlas, so a frame allocates
// nothing (CPP10). The zoom is a look at the text, not the view, so it stays out of the
// manifest, in .vulpen/font.ini in the node's folder, which a restart reads, and every
// part that lays text out in cells grows with it.
class Font final : public VP::Operator {
  // The ports first, so a face refused leaves no other error.
  void bind(VP::Bind &node) override {
    _atlas = node.texture<std::uint8_t>("atlas");
    _font = node.upload<VP_VIEW::Font>("font", 1);
    _metrics = &node.output<VP_VIEW::Font>("metrics");
    _zoom_in = node.command("zoom in",
                            "draws the text a step larger, and with it every part that "
                            "lays text out in cells");
    _zoom_out = node.command("zoom out", "draws the text a step smaller");
    _zoom_reset = node.command("zoom reset", "draws the text at the font's height again");
    _kept = node.file(kept_file);
    _height = node.param<std::uint32_t>("height");
    _face = node.param<std::string>("face");
    _baked = {};
    // In its folder, so the font moves with its view (V03).
    if (_face.find_first_of("/\\") != std::string::npos)
      throw std::runtime_error(std::format(
          "param face = {}: a file of the node's folder, named without a folder", _face));
    if (!_face.empty()) // the loader names a face left unset
      _file = node.file(_face);
  }
  // Written every frame, as it is a few bytes, so a buffer made anew for a connection
  // holds the Font too. Shaders read it from the buffer, and C++ that lays text out
  // from metrics, so both measure with the font that draws.
  void cook(VP::Cook &frame) override {
    look(frame);
    if (_baked.count == 0 || _baked.cell.y != height() || frame.empty(_atlas))
      _baked = bake(frame);
    frame.write(_font).front() = _baked;
    *_metrics = _baked;
  }

  // The zoom the .ini holds, read again once it changes on disk. One that holds a
  // mistake is named in the log, and the font draws at its height.
  void look(VP::Cook &frame) {
    const std::string_view kept = frame.files().text(_kept);
    if (_read && kept == *_read)
      return;
    _read.emplace(kept);
    try {
      _zoom = zoom_of(kept);
    } catch (const std::runtime_error &mistake) {
      _zoom = 1.0f;
      frame.log(VP::Level::error,
                std::format("{}; the font draws at its height", mistake.what()));
    }
  }

  // From what the .ini holds now, so two zooms in a frame make two steps, written whole
  // (RA04); the font draws it from its next frame. An .ini that holds a mistake refuses
  // it, naming the line, so no zoom writes over what a person typed.
  void command(VP::Call &call) override {
    const float zoom = zoom_of(call.files().text(_kept));
    float to = 1.0f;
    if (call.is(_zoom_in))
      to = std::min(zoom * zoom_step, zoom_most);
    else if (call.is(_zoom_out))
      to = std::max(zoom / zoom_step, zoom_least);
    call.files().save(_kept, std::format("{}zoom = {:.3f}\n", kept_header, to));
  }

  // At least a pixel for a height of one, as zoom_least is a half; 0 stays 0, which
  // bake refuses.
  std::uint32_t height() const {
    return static_cast<std::uint32_t>(std::lround(static_cast<float>(_height) * _zoom));
  }
  VP_VIEW::Font bake(VP::Cook &frame) const;

  VP::Texture<std::uint8_t> _atlas;
  VP::Upload<VP_VIEW::Font> _font;
  VP_VIEW::Font *_metrics = nullptr;
  VP::Command _zoom_in, _zoom_out, _zoom_reset;
  VP::File _kept;
  std::optional<std::string> _read; // the .ini the zoom is from
  float _zoom = 1.0f;
  std::uint32_t _height = 0;
  std::string _face;
  VP::File _file;
  VP_VIEW::Font _baked;
};

// A cell is as wide as the font's advance and as tall as the height param times the
// zoom, its baseline where the font's ascent puts it. Ink past a cell's edge is cut, so
// no glyph reaches into its neighbor's cell.
VP_VIEW::Font Font::bake(VP::Cook &frame) const {
  const std::string_view bytes = frame.files().text(_file);
  if (bytes.empty())
    throw std::runtime_error(
        std::format("face {} is no file of the node's folder, or an empty one", _face));
  const auto *const data = reinterpret_cast<const unsigned char *>(bytes.data());
  stbtt_fontinfo font{};
  if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0)))
    throw std::runtime_error(std::format("face {} is no font stb_truetype reads", _face));
  const std::uint32_t tall = height();
  const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(tall));
  int ascent = 0, descent = 0, gap = 0, advance = 0;
  stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
  stbtt_GetCodepointHMetrics(&font, first, &advance, nullptr);
  const glm::ivec2 cell(std::lround(static_cast<float>(advance) * scale), tall);
  if (cell.x <= 0 || cell.y <= 0)
    throw std::runtime_error(std::format("height = {} makes a cell of no size", tall));
  const glm::ivec2 size = cell * glm::ivec2(columns, rows);
  const int baseline = static_cast<int>(std::lround(static_cast<float>(ascent) * scale));
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size.x) * size.y);
  std::vector<unsigned char> ink;
  for (std::uint32_t glyph = 0; glyph < count; ++glyph) {
    const int code = static_cast<int>(first + glyph);
    int wide = 0;
    stbtt_GetCodepointHMetrics(&font, code, &wide, nullptr);
    if (wide != advance)
      throw std::runtime_error(std::format(
          "face {} is not monospace: {:c} is not as wide as the space", _face, code));
    glm::ivec2 low{}, high{};
    stbtt_GetCodepointBitmapBox(
        &font, code, scale, scale, &low.x, &low.y, &high.x, &high.y);
    const glm::ivec2 box = high - low;
    ink.assign(static_cast<std::size_t>(box.x) * box.y, 0);
    stbtt_MakeCodepointBitmap(&font, ink.data(), box.x, box.y, box.x, scale, scale, code);
    const glm::ivec2 corner = cell * glm::ivec2(glyph % columns, glyph / columns);
    for (int y = 0; y < box.y; ++y)
      for (int x = 0; x < box.x; ++x) {
        const glm::ivec2 at(low.x + x, baseline + low.y + y);
        if (at.x >= 0 && at.y >= 0 && at.x < cell.x && at.y < cell.y)
          pixels[static_cast<std::size_t>(corner.y + at.y) * size.x + corner.x + at.x] =
              ink[static_cast<std::size_t>(y) * box.x + x];
      }
  }
  frame.upload(_atlas, pixels, glm::uvec2(size));
  return {.cell = glm::uvec2(cell),
          .atlas = glm::uvec2(size),
          .columns = columns,
          .first = first,
          .count = count};
}

} // namespace

VP_OPERATORS(registry) {
  registry.add<Font>("Font");
}
