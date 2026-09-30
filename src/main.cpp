#include "commit.h"
#include "runtime/Runtime.h"

#include <cstdio>
#include <exception>
#include <span>

int main(int argc, char **argv) {
  constexpr const char *build = "vulpen " VULPEN_COMMIT;
  try {
    VP::Runtime runtime(std::span<char *const>(argv, static_cast<std::size_t>(argc)),
                        build);
    return runtime.run();
  } catch (const std::exception &error) {
    // Only arguments vulpen cannot run with reach here; the runtime logs the rest.
    std::fprintf(stderr, "%s: %s\n", build, error.what());
    return 1;
  }
}
