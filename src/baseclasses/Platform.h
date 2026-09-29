#pragma once

#include <filesystem>

namespace VP {

// The only place OS APIs and OS #ifdefs appear, so everything above stays portable.
class Files {
public:
  // Build outputs resolve against it, never against the working directory (RP02).
  static std::filesystem::path executable();
};

// Machine code loaded while running. Only a dev build loads recipe C++ this way; a
// release build links the same code in.
class Library {
public:
  explicit Library(const std::filesystem::path &file);
  Library(Library &&other) noexcept;
  Library &operator=(Library &&other) noexcept;
  ~Library();

  void *symbol(const char *name) const;
  // Whether the OS still maps the file after every Library of it is gone; a new load of
  // the same path would then return the old code.
  static bool mapped(const std::filesystem::path &file);

private:
  void *_handle;
};

class Window {};
class Terminal {};

} // namespace VP
