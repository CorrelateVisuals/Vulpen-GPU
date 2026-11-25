#include "tools/bench/Bench.h"

#include <atomic>
#include <cstdlib>
#include <new>

// A bench runs the functions its case file registers, as bench.py asks. A sanitizer's
// build counts no allocation: the sanitizer's own allocation functions stand in for a
// program's, and counting replacements of some of them would leave it others to pair
// with free, which it reports as a mismatch.

namespace VP::bench {

namespace {

// The bench's one mutable global (C13): the allocation functions a program replaces are
// global, so what they count is too.
struct Counted {
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> bytes{0};
};
Counted counted;

} // namespace

Allocated allocated() {
  return {counted.count.load(), counted.bytes.load()};
}

} // namespace VP::bench

#if VP_BENCH_COUNTS

void *operator new(std::size_t size) {
  using VP::bench::counted;
  counted.count.fetch_add(1, std::memory_order_relaxed);
  counted.bytes.fetch_add(size, std::memory_order_relaxed);
  if (void *memory = std::malloc(size == 0 ? 1 : size))
    return memory;
  throw std::bad_alloc();
}

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}
#endif

int main(int argc, char **argv) {
  VP::bench::Bench bench;
  VP::bench::cases(bench);
  return bench.run(std::span<char *const>(argv, static_cast<std::size_t>(argc)));
}
