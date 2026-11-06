#pragma once

#include <cstdint>
#include <string_view>

// FNV-1a over 64 bits: the same number on every machine and compiler (C01), so a
// fingerprint a manifest or a kept file holds reads the same in every build. Each user
// frames what it hashes itself, as the fingerprints already written must not change.
namespace {

constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

// The hash with the bytes of the text added.
constexpr std::uint64_t hashed(std::uint64_t hash, std::string_view text) {
  for (const char byte : text)
    hash = (hash ^ static_cast<unsigned char>(byte)) * fnv_prime;
  return hash;
}

} // namespace
