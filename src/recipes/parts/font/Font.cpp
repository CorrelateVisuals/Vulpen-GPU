#include "contracts/Font.h"
#include "runtime/Operator.h"

// Static, so each view's copy of this part keeps stb_truetype to itself, and copies in
// two views link into one release binary.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
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

// Bakes the monospace font file its face param names at the height its height param
// gives, a cell a glyph, into an atlas that glyphs samples a texel a pixel. It bakes
// after it binds, as an edit of either param makes it do, and once image clear empties
// the atlas, so a frame allocates nothing (CPP10).
class Font final : public VP::Operator {
  // The ports first, so a face refused leaves no other error.
  void bind(VP::Bind &node) override {
    _atlas = node.texture<std::uint8_t>("atlas");
    _font = node.upload<VP_VIEW::Font>("font", 1);
    _metrics = &node.output<VP_VIEW::Font>("metrics");
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
    if (_baked.count == 0 || frame.empty(_atlas))
      _baked = bake(frame);
    frame.write(_font).front() = _baked;
    *_metrics = _baked;
  }
  VP_VIEW::Font bake(VP::Cook &frame) const;

  VP::Texture<std::uint8_t> _atlas;
  VP::Upload<VP_VIEW::Font> _font;
  VP_VIEW::Font *_metrics = nullptr;
  std::uint32_t _height = 0;
  std::string _face;
  VP::File _file;
  VP_VIEW::Font _baked;
};

// A cell is as wide as the font's advance and as tall as the height param, its baseline
// where the font's ascent puts it. Ink past a cell's edge is cut, so no glyph reaches
// into its neighbor's cell.
VP_VIEW::Font Font::bake(VP::Cook &frame) const {
  const std::string_view bytes = frame.files().text(_file);
  if (bytes.empty())
    throw std::runtime_error(
        std::format("face {} is no file of the node's folder, or an empty one", _face));
  const auto *const data = reinterpret_cast<const unsigned char *>(bytes.data());
  stbtt_fontinfo font{};
  if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0)))
    throw std::runtime_error(std::format("face {} is no font stb_truetype reads", _face));
  const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(_height));
  int ascent = 0, descent = 0, gap = 0, advance = 0;
  stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
  stbtt_GetCodepointHMetrics(&font, first, &advance, nullptr);
  const glm::ivec2 cell(std::lround(static_cast<float>(advance) * scale), _height);
  if (cell.x <= 0 || cell.y <= 0)
    throw std::runtime_error(std::format("height = {} makes a cell of no size", _height));
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
