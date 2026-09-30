#include "baseclasses/Platform.h"

#include <GLFW/glfw3.h>
#include <time.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace VP {

namespace {

[[noreturn]] void glfw_failed(const char *call) {
  const char *description = nullptr;
  glfwGetError(&description);
  throw std::runtime_error(
      std::format("{} failed: {}", call, description ? description : "no reason given"));
}

} // namespace

#ifdef _WIN32

namespace {

constexpr DWORD longest_path = 32767; // in UTF-16 units, with long paths enabled

std::system_error last_error(const std::string &what) {
  return {static_cast<int>(GetLastError()), std::system_category(), what};
}

// One per process and module, so two runs of one build never share a copy.
std::filesystem::path loaded_copy(const std::filesystem::path &file) {
  return std::filesystem::temp_directory_path() /
         std::format("vulpen-{}-{:016x}{}",
                     GetCurrentProcessId(),
                     std::filesystem::hash_value(file),
                     file.extension().string());
}

} // namespace

std::filesystem::path Files::executable() {
  std::wstring file(longest_path, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, file.data(), longest_path);
  if (length == 0)
    throw last_error("GetModuleFileNameW");
  file.resize(length);
  return file;
}

std::tm local_time(std::time_t time) {
  std::tm local{};
  localtime_s(&local, &time);
  return local;
}

// A console shows ANSI colors once asked to; a pipe or a file has no console mode.
bool Terminal::colors() {
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  return GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0) <= 1 && // unset or empty
         GetConsoleMode(out, &mode) &&
         SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

Library::Library(const std::filesystem::path &file) : _copy(loaded_copy(file)) {
  std::filesystem::copy_file(
      file, _copy, std::filesystem::copy_options::overwrite_existing);
  _handle = LoadLibraryW(_copy.c_str());
  if (!_handle) {
    const std::system_error failure = last_error(file.string());
    std::error_code ignored; // the load's error is the one to report
    std::filesystem::remove(_copy, ignored);
    throw failure;
  }
}

Library::~Library() {
  if (!_handle)
    return;
  FreeLibrary(static_cast<HMODULE>(_handle));
  // Fails while the OS still maps the copy, which the next load of the module reports.
  std::error_code mapped;
  std::filesystem::remove(_copy, mapped);
}

void *Library::symbol(const char *name) const {
  return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(_handle), name));
}

bool Library::mapped(const std::filesystem::path &file) {
  return GetModuleHandleW(loaded_copy(file).c_str()) != nullptr;
}

// cmd /c strips the first and the last quote from a line that starts with one.
int Shell::run(const std::string &command) {
  return std::system(std::format("\"{}\"", command).c_str());
}

#else

std::filesystem::path Files::executable() {
  return std::filesystem::read_symlink("/proc/self/exe");
}

std::tm local_time(std::time_t time) {
  std::tm local{};
  localtime_r(&time, &local);
  return local;
}

bool Terminal::colors() {
  const char *const no_color = std::getenv("NO_COLOR");
  const char *const term = std::getenv("TERM");
  return isatty(STDOUT_FILENO) == 1 && !(no_color && *no_color) &&
         !(term && std::string_view(term) == "dumb");
}

Library::Library(const std::filesystem::path &file)
    : _handle(dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL)) {
  if (!_handle)
    throw std::runtime_error(dlerror());
}

Library::~Library() {
  if (_handle)
    dlclose(_handle);
}

void *Library::symbol(const char *name) const {
  return dlsym(_handle, name);
}

bool Library::mapped(const std::filesystem::path &file) {
  void *const handle = dlopen(file.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (handle)
    dlclose(handle);
  return handle != nullptr;
}

int Shell::run(const std::string &command) {
  return std::system(command.c_str());
}

#endif

Library::Library(Library &&other) noexcept
    : _handle(std::exchange(other._handle, nullptr)), _copy(std::move(other._copy)) {}

Library &Library::operator=(Library &&other) noexcept {
  std::swap(_handle, other._handle);
  std::swap(_copy, other._copy);
  return *this;
}

// GLFW keeps process-wide state, so the one Window owns its init and termination.
Window::Window(const std::string &title, VkExtent2D size) {
  // GLFW then finds Vulkan through the loader vulpen links, not a second one.
  glfwInitVulkanLoader(vkGetInstanceProcAddr);
  if (!glfwInit())
    glfw_failed("glfwInit");
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  _window = glfwCreateWindow(static_cast<int>(size.width),
                             static_cast<int>(size.height),
                             title.c_str(),
                             nullptr,
                             nullptr);
  if (!_window) {
    glfwTerminate();
    glfw_failed("glfwCreateWindow");
  }
}

Window::~Window() {
  glfwDestroyWindow(_window);
  glfwTerminate();
}

bool Window::poll() const {
  glfwPollEvents();
  return !glfwWindowShouldClose(_window);
}

VkExtent2D Window::size() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(_window, &width, &height);
  return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

std::span<const char *const> Window::vulkan_extensions() const {
  std::uint32_t count = 0;
  const char **const names = glfwGetRequiredInstanceExtensions(&count);
  if (!names)
    glfw_failed("glfwGetRequiredInstanceExtensions");
  return {names, count};
}

VkSurfaceKHR Window::surface(VkInstance instance) const {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(instance, _window, nullptr, &surface) != VK_SUCCESS)
    glfw_failed("glfwCreateWindowSurface");
  return surface;
}

} // namespace VP
