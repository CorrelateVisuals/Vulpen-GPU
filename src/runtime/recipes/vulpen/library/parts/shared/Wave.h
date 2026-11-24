#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// WAV files (RIFF, little-endian), as the sound parts read and write them: samples as
// 32-bit floats, two channels interleaved, left then right, at the rate the sound port
// plays, which a file of another is refused naming.
namespace {

// "And his feet like unto fine brass, as if they burned in a furnace; and his voice as
// the sound of many waters." Revelation 1:15

constexpr std::uint32_t wave_rate = 48000;
constexpr std::uint32_t wave_channels = 2;
constexpr std::uint16_t wave_pcm = 1;   // integer samples
constexpr std::uint16_t wave_float = 3; // IEEE 754 samples
// A format that names its own in a sub-format, as wider samples are often written: its
// first two bytes are the format, at extensible_format in an fmt chunk of extended size.
constexpr std::uint16_t wave_extensible = 0xFFFE;
constexpr std::size_t extended_format_bytes = 40;
constexpr std::size_t extensible_format = 24;
constexpr std::uint32_t wave_header = 44; // RIFF, fmt and data heads, as written
constexpr std::uint32_t wave_format_bytes = 16;
// The RIFF layout: a file opens with RIFF, the size after it and WAVE, then chunks, each
// an id of four letters and its size before its body. Its fields are little-endian, of
// two bytes or four. In the fmt chunk's body, where the channels, the rate and the bits
// of a sample lie, after the format.
constexpr std::size_t riff_id = 4;
constexpr std::size_t riff_kind = 8;    // where WAVE lies
constexpr std::size_t riff_chunks = 12; // where the first chunk starts
constexpr std::size_t chunk_head = 8;   // an id and a size
constexpr std::size_t short_field = sizeof(std::uint16_t);
constexpr std::size_t long_field = sizeof(std::uint32_t);
constexpr std::size_t fmt_channels = 2;
constexpr std::size_t fmt_rate = 4;
constexpr std::size_t fmt_bits = 14;

inline std::uint32_t wave_word(std::string_view bytes, std::size_t at, std::size_t size) {
  std::uint32_t word = 0;
  for (std::size_t byte = 0; byte < size; ++byte)
    word |= std::uint32_t{static_cast<unsigned char>(bytes[at + byte])} << (8 * byte);
  return word;
}

// One sample, as a float from -1 to 1: 16- and 24-bit integers by their range, floats as
// they are.
inline float wave_sample(std::string_view bytes,
                         std::size_t at,
                         std::uint16_t format,
                         std::uint16_t bits) {
  if (format == wave_float) {
    const std::uint32_t word = wave_word(bytes, at, sizeof(float));
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof value);
    return value;
  }
  const std::uint32_t word = wave_word(bytes, at, bits / 8);
  const std::uint32_t sign = 1U << (bits - 1);
  const auto value = static_cast<std::int32_t>((word ^ sign) - sign); // sign extended
  return static_cast<float>(value) / static_cast<float>(sign);
}

// The samples of a WAV file's bytes, two channels, a mono file's in both. Throws naming
// the file and what of it this reads not: 16-bit or 24-bit integers or 32-bit floats,
// one or two channels, at the port's rate.
inline std::vector<float> wave_read(std::string_view bytes, std::string_view file) {
  const auto refuse = [&](std::string_view why) {
    return std::runtime_error(std::format("{} is no WAV file this reads: {}", file, why));
  };
  if (bytes.size() < riff_chunks || bytes.substr(0, riff_id) != "RIFF" ||
      bytes.substr(riff_kind, riff_id) != "WAVE")
    throw refuse("it holds no RIFF WAVE header");
  std::uint16_t format = 0, channels = 0, bits = 0;
  std::uint32_t rate = 0;
  for (std::size_t at = riff_chunks; at + chunk_head <= bytes.size();) {
    const std::string_view id = bytes.substr(at, riff_id);
    const std::size_t size = wave_word(bytes, at + riff_id, long_field);
    const std::size_t body = at + chunk_head;
    if (body + size > bytes.size())
      throw refuse(std::format("its {} chunk runs past its end", id));
    if (id == "fmt " && size >= wave_format_bytes) {
      format = static_cast<std::uint16_t>(wave_word(bytes, body, short_field));
      if (format == wave_extensible && size >= extended_format_bytes)
        format = static_cast<std::uint16_t>(
            wave_word(bytes, body + extensible_format, short_field));
      channels =
          static_cast<std::uint16_t>(wave_word(bytes, body + fmt_channels, short_field));
      rate = wave_word(bytes, body + fmt_rate, long_field);
      bits = static_cast<std::uint16_t>(wave_word(bytes, body + fmt_bits, short_field));
    } else if (id == "data") {
      const bool integers = format == wave_pcm && (bits == 16 || bits == 24);
      if (!integers && !(format == wave_float && bits == 32))
        throw refuse(std::format("its samples are format {} of {} bits", format, bits));
      if (channels != 1 && channels != wave_channels)
        throw refuse(std::format("it has {} channels", channels));
      if (rate != wave_rate)
        throw refuse(std::format("it plays at {} Hz, not {} Hz", rate, wave_rate));
      const std::size_t step = bits / 8;
      std::vector<float> samples;
      samples.reserve(size / step / channels * wave_channels);
      for (std::size_t sample = body; sample + step * channels <= body + size;
           sample += step * channels)
        for (std::uint32_t channel = 0; channel < wave_channels; ++channel)
          samples.push_back(wave_sample(
              bytes, sample + step * (channels == 1 ? 0 : channel), format, bits));
      return samples;
    }
    at = body + size + size % 2; // a chunk of an odd size is padded to an even one
  }
  throw refuse("it holds no data chunk");
}

// A WAV file of the samples, two channels of 32-bit floats at the port's rate.
inline std::string wave_write(std::span<const float> samples) {
  std::string bytes;
  const auto word = [&](std::uint32_t value, std::size_t size) {
    for (std::size_t byte = 0; byte < size; ++byte)
      bytes.push_back(static_cast<char>((value >> (8 * byte)) & 0xFFU));
  };
  const auto data = static_cast<std::uint32_t>(samples.size() * sizeof(float));
  const std::uint32_t block = wave_channels * sizeof(float);
  bytes.append("RIFF");
  word(wave_header - chunk_head + data, long_field);
  bytes.append("WAVEfmt ");
  word(wave_format_bytes, long_field);
  word(wave_float, short_field);
  word(wave_channels, short_field);
  word(wave_rate, long_field);
  word(wave_rate * block, long_field);
  word(block, short_field);
  word(8 * sizeof(float), short_field);
  bytes.append("data");
  word(data, long_field);
  for (const float sample : samples) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &sample, sizeof bits);
    word(bits, sizeof bits);
  }
  return bytes;
}

} // namespace
