#include "shared/Hash.h"

#include "tools/bench/Bench.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// The library's shared/Hash.h: FNV-1a over 64 bits, which fingerprints a manifest and a
// kept file hold, so one changed row would make every one written read as changed.

namespace VP::bench {

void cases(Bench &bench) {
  bench.sweep("Hash.h/hashed", hashed);
  // The values FNV's authors publish, so it is FNV-1a, not only what it was when first
  // recorded.
  bench.check("Hash.h/hashed", [](Rows &rows) {
    for (const std::string_view text : {"", "a", "foobar"})
      rows.row(std::format("FNV-1a of {} from the offset -> {:016x}",
                           escaped(text),
                           hashed(fnv_offset, text)));
  });
  bench.holds(
      "Hash.h/hashed",
      "two texts in turn hash as the two joined",
      [](std::uint64_t hash, const std::string &first, const std::string &second) {
        return hashed(hashed(hash, first), second) == hashed(hash, first + second);
      });
}

} // namespace VP::bench
