#include "commit.h"
#include "runtime/Runtime.h"

#include <cstdio>
#include <exception>
#include <span>

int main(int argc, char **argv) {
  std::puts("vulpen " VULPEN_COMMIT);
  try {
    VP::Runtime runtime(std::span<char *const>(argv, static_cast<std::size_t>(argc)));
    return runtime.run();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
