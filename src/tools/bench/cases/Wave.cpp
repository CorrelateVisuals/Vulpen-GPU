#include "shared/Wave.h"

#include "tools/bench/Bench.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's shared/Wave.h: WAV files as the sound parts read and write them. The
// files are written here chunk by chunk, as other programs write them: floats and 16- and
// 24-bit integers, one channel or two, the extensible format, chunks the parts pass over,
// and each way a file can be other than this reads.

namespace VP::bench {

namespace {

// A field of a file, little-endian.
std::string field(std::uint32_t value, std::size_t size) {
  std::string bytes;
  for (std::size_t byte = 0; byte < size; ++byte)
    bytes.push_back(static_cast<char>((value >> (8 * byte)) & 0xFFU));
  return bytes;
}

// A chunk, its size as its body's unless said, padded to an even size.
std::string chunk(std::string_view id,
                  std::string_view body,
                  std::optional<std::uint32_t> size = std::nullopt) {
  return std::string(id) +
         field(size.value_or(static_cast<std::uint32_t>(body.size())), 4) +
         std::string(body) + (body.size() % 2 ? std::string(1, '\0') : "");
}

std::string riff(std::string_view chunks) {
  return "RIFF" + field(static_cast<std::uint32_t>(4 + chunks.size()), 4) + "WAVE" +
         std::string(chunks);
}

std::string fmt(std::uint16_t format,
                std::uint16_t channels,
                std::uint32_t rate,
                std::uint16_t bits) {
  const std::uint32_t block = channels * bits / 8U;
  return chunk("fmt ",
               field(format, 2) + field(channels, 2) + field(rate, 4) +
                   field(rate * block, 4) + field(block, 2) + field(bits, 2));
}

// The extensible format, its sub-format's two bytes first in its GUID.
std::string extensible(std::uint16_t format,
                       std::uint16_t channels,
                       std::uint32_t rate,
                       std::uint16_t bits) {
  const std::uint32_t block = channels * bits / 8U;
  return chunk(
      "fmt ",
      field(0xFFFE, 2) + field(channels, 2) + field(rate, 4) + field(rate * block, 4) +
          field(block, 2) + field(bits, 2) + field(22, 2) + field(bits, 2) + field(3, 4) +
          field(format, 2) +
          std::string("\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14));
}

std::string floats(std::initializer_list<float> samples) {
  std::string bytes;
  for (const float sample : samples)
    bytes += field(std::bit_cast<std::uint32_t>(sample), 4);
  return bytes;
}

std::string integers(std::initializer_list<std::int32_t> samples, std::size_t size) {
  std::string bytes;
  for (const std::int32_t sample : samples)
    bytes += field(static_cast<std::uint32_t>(sample), size);
  return bytes;
}

std::vector<std::pair<std::string, std::string>> files() {
  const std::string stereo = floats({0.5f, -0.5f, 1.0f, -1.0f});
  return {
      {"floats", riff(fmt(3, 2, 48000, 32) + chunk("data", stereo))},
      {"as written", wave_write(std::vector<float>{0.25f, -0.25f, 0.0f, 1.0f})},
      {"16-bit mono",
       riff(fmt(1, 1, 48000, 16) + chunk("data", integers({32767, -32768, 0, 1}, 2)))},
      {"16-bit stereo",
       riff(fmt(1, 2, 48000, 16) + chunk("data", integers({16384, -16384}, 2)))},
      {"24-bit stereo",
       riff(fmt(1, 2, 48000, 24) +
            chunk("data", integers({8388607, -8388608, 4194304, -1}, 3)))},
      {"extensible floats", riff(extensible(3, 2, 48000, 32) + chunk("data", stereo))},
      {"extensible 24-bit",
       riff(extensible(1, 2, 48000, 24) +
            chunk("data", integers({8388607, -8388608}, 3)))},
      {"a list chunk of odd size first",
       riff(fmt(3, 2, 48000, 32) + chunk("LIST", "abc") + chunk("data", stereo))},
      {"a frame cut off at the end",
       riff(fmt(1, 2, 48000, 16) + chunk("data", integers({1, 2, 3}, 2)))},
      {"44100 Hz", riff(fmt(3, 2, 44100, 32) + chunk("data", stereo))},
      {"three channels", riff(fmt(3, 3, 48000, 32) + chunk("data", stereo))},
      {"no channels", riff(fmt(3, 0, 48000, 32) + chunk("data", stereo))},
      {"8-bit", riff(fmt(1, 2, 48000, 8) + chunk("data", "abcd"))},
      {"32-bit integers", riff(fmt(1, 2, 48000, 32) + chunk("data", stereo))},
      {"64-bit floats", riff(fmt(3, 2, 48000, 64) + chunk("data", stereo))},
      {"data before fmt", riff(chunk("data", stereo) + fmt(3, 2, 48000, 32))},
      {"no data", riff(fmt(3, 2, 48000, 32))},
      {"a short fmt",
       riff(chunk("fmt ", std::string("\x03\x00\x02\x00", 4)) + chunk("data", stereo))},
      {"extensible of too short a size",
       riff(chunk("fmt ", fmt(3, 2, 48000, 32).substr(8).replace(0, 2, "\xFE\xFF")) +
            chunk("data", stereo))},
      {"data past the end",
       riff(fmt(3, 2, 48000, 32) + chunk("data", stereo, 0xFFFFFFFFU))},
      {"a size past the end of a chunk passed over",
       riff(fmt(3, 2, 48000, 32) + chunk("LIST", "ab", 4000))},
      {"no WAVE", "RIFF" + field(4, 4) + "WAVX"},
      {"empty", ""},
      {"RIFF alone", "RIFF"}};
}

} // namespace

void cases(Bench &bench) {
  bench.check("Wave.h/wave_read", [](Rows &rows) {
    for (const auto &[what, bytes] : files()) {
      std::string said;
      try {
        said = show(wave_read(bytes, "a.wav"), rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", what, said));
    }
  });
  bench.sweep("Wave.h/wave_read", wave_read, domain::FileText{}, domain::Name{});

  bench.sweep("Wave.h/wave_write",
              [](const std::vector<float> &samples) { return wave_write(samples); });
  bench.holds(
      "Wave.h/wave_write",
      "what it writes reads back bit for bit, while its samples make whole frames",
      [](const std::vector<float> &samples) {
        if (samples.size() % 2 != 0)
          return true;
        const std::vector<float> read = wave_read(wave_write(samples), "a.wav");
        return read.size() == samples.size() &&
               std::equal(
                   read.begin(), read.end(), samples.begin(), [](float a, float b) {
                     return std::bit_cast<std::uint32_t>(a) ==
                            std::bit_cast<std::uint32_t>(b);
                   });
      });

  // Each field's size at each place in a few bytes, as wave_read reads them only where
  // they lie within the file.
  bench.check("Wave.h/wave_word", [](Rows &rows) {
    const std::string bytes("\x01\x02\x80\xFF\x00\x7F", 6);
    for (const std::size_t size : {1, 2, 3, 4})
      for (std::size_t at = 0; at + size <= bytes.size(); ++at)
        rows.row(
            std::format("{} bytes at {} -> {:#x}", size, at, wave_word(bytes, at, size)));
  });
  bench.check("Wave.h/wave_sample", [](Rows &rows) {
    const std::vector<std::pair<std::string, std::uint16_t>> samples{
        {integers({32767}, 2), 16},
        {integers({-32768}, 2), 16},
        {integers({0}, 2), 16},
        {integers({-1}, 2), 16},
        {integers({8388607}, 3), 24},
        {integers({-8388608}, 3), 24},
        {integers({1}, 3), 24},
        {floats({0.75f}), 32},
        {floats({-2.0f}), 32}};
    for (const auto &[bytes, bits] : samples) {
      const std::uint16_t format = bits == 32 ? wave_float : wave_pcm;
      rows.row(std::format("{} of format {} and {} bits -> {}",
                           escaped(bytes),
                           format,
                           bits,
                           number_text(wave_sample(bytes, 0, format, bits))));
    }
  });
}

} // namespace VP::bench
