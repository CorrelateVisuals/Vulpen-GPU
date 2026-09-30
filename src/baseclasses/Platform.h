#pragma once

#include <vulkan/vulkan.h>

#include <ctime>
#include <filesystem>
#include <span>
#include <string>

struct GLFWwindow;

namespace VP {

// The only place OS APIs and OS #ifdefs appear, so everything above stays portable.
class Files {
public:
  // Build outputs resolve against it, never against the working directory (RP02).
  static std::filesystem::path executable();
};

// The wall clock in the machine's time zone, for the people who read the log.
std::tm local_time(std::time_t time);

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
  void *_handle = nullptr;
  // Windows locks a loaded DLL, and the build must replace the file (rule 7 of
  // docs/plans/live-code.md), so there the OS maps this copy of it instead; else empty.
  std::filesystem::path _copy;
};

// The shell that runs a command line, as std::system does.
class Shell {
public:
  // Waits for the command; 0 when it succeeded.
  static int run(const std::string &command);
};

// A desktop window through GLFW. Only a view that draws opens one (V07).
class Window {
public:
  // Throws, naming why, when no display can hold a window.
  Window(const std::string &title, VkExtent2D size);
  ~Window();
  Window(const Window &) = delete;
  Window &operator=(const Window &) = delete;

  // Takes the events since the last call; false once the window was asked to close.
  bool poll() const;
  // In pixels; zero while the window is minimized.
  VkExtent2D size() const;
  // What a Vulkan instance enables to present to this window.
  std::span<const char *const> vulkan_extensions() const;
  // The caller owns the surface and destroys it before the instance.
  VkSurfaceKHR surface(VkInstance instance) const;

private:
  GLFWwindow *_window = nullptr;
};

// The terminal vulpen writes to, when it runs in one.
class Terminal {
public:
  // Whether standard output shows color: a terminal that is not dumb, with no NO_COLOR
  // set (no-color.org).
  static bool colors();
};

} // namespace VP
