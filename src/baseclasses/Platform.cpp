#include "baseclasses/Platform.h"

#include <GLFW/glfw3.h>
#include <time.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace VP {

namespace {

constexpr std::size_t input_chunk = 4096; // bytes of standard input one read takes
// At most this much input a frame, so input that never pauses cannot hold a frame.
constexpr std::size_t input_per_call = 16 * input_chunk;

[[noreturn]] void glfw_failed(const char *call) {
  const char *description = nullptr;
  glfwGetError(&description);
  throw std::runtime_error(
      std::format("{} failed: {}", call, description ? description : "no reason given"));
}

// The keys a layout prints no character for, by GLFW's code; F1 to F12 follow on.
constexpr std::array<std::pair<int, std::string_view>, 24> named_keys{
    {{GLFW_KEY_SPACE, "space"},
     {GLFW_KEY_ENTER, "enter"},
     {GLFW_KEY_KP_ENTER, "enter"},
     {GLFW_KEY_ESCAPE, "escape"},
     {GLFW_KEY_TAB, "tab"},
     {GLFW_KEY_BACKSPACE, "backspace"},
     {GLFW_KEY_INSERT, "insert"},
     {GLFW_KEY_DELETE, "delete"},
     {GLFW_KEY_LEFT, "left"},
     {GLFW_KEY_RIGHT, "right"},
     {GLFW_KEY_UP, "up"},
     {GLFW_KEY_DOWN, "down"},
     {GLFW_KEY_PAGE_UP, "page_up"},
     {GLFW_KEY_PAGE_DOWN, "page_down"},
     {GLFW_KEY_HOME, "home"},
     {GLFW_KEY_END, "end"},
     {GLFW_KEY_LEFT_SHIFT, "shift"},
     {GLFW_KEY_RIGHT_SHIFT, "shift"},
     {GLFW_KEY_LEFT_CONTROL, "control"},
     {GLFW_KEY_RIGHT_CONTROL, "control"},
     {GLFW_KEY_LEFT_ALT, "alt"},
     {GLFW_KEY_RIGHT_ALT, "alt"},
     {GLFW_KEY_LEFT_SUPER, "super"},
     {GLFW_KEY_RIGHT_SUPER, "super"}}};
constexpr std::array<std::string_view, 12> function_keys{
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12"};
// GLFW_MOUSE_BUTTON_LEFT, RIGHT and MIDDLE, in that order.
constexpr std::array<std::string_view, 3> buttons{"left", "right", "middle"};
// UTF-8 (RFC 3629): a character past ASCII takes a first byte that marks how many bytes
// follow it, each carrying six more bits of the character.
constexpr std::uint32_t ascii_end = 0x80;
constexpr std::uint32_t continuation = 0x80;
constexpr std::uint32_t continuation_bits = 6;
constexpr std::uint32_t continuation_mask = (1u << continuation_bits) - 1;
// By the character's length in bytes: the largest it holds, and its first byte's mark.
constexpr std::array<std::uint32_t, 3> length_ends{0x800, 0x10000, 0x110000};
constexpr std::array<std::uint32_t, 3> first_marks{0xC0, 0xE0, 0xF0};

std::size_t utf8_length(unsigned char first) {
  if (first < ascii_end)
    return 1;
  const auto mark = std::ranges::find_if(first_marks, [&](std::uint32_t marked) {
    return (first & (marked | marked >> 1)) == marked;
  });
  // A byte that only continues a character starts none.
  return mark == first_marks.end()
             ? 0
             : 2 + static_cast<std::size_t>(mark - first_marks.begin());
}

std::string utf8(std::uint32_t point) {
  if (point < ascii_end)
    return std::string(1, static_cast<char>(point));
  const auto end = std::ranges::find_if(
      length_ends, [&](std::uint32_t largest) { return point < largest; });
  const auto extra = static_cast<std::size_t>(end - length_ends.begin()) + 1;
  std::string text(extra + 1, '\0');
  for (std::size_t at = extra; at > 0; --at, point >>= continuation_bits)
    text[at] = static_cast<char>(continuation | (point & continuation_mask));
  text[0] = static_cast<char>(first_marks[extra - 1] | point);
  return text;
}

// Empty for a key with no name, which the window hands on no event for.
std::string key_name(int key, int scancode) {
  const auto named =
      std::ranges::find(named_keys, key, &decltype(named_keys)::value_type::first);
  if (named != named_keys.end())
    return std::string(named->second);
  if (key >= GLFW_KEY_F1 && key < GLFW_KEY_F1 + static_cast<int>(function_keys.size()))
    return std::string(function_keys[static_cast<std::size_t>(key - GLFW_KEY_F1)]);
  const char *const printed = glfwGetKeyName(key, scancode);
  return printed ? printed : "";
}

// Set only while the window polls, so no callback outlives its input (C13).
WindowInput *input_of(GLFWwindow *window) {
  return static_cast<WindowInput *>(glfwGetWindowUserPointer(window));
}

void on_key(GLFWwindow *window, int key, int scancode, int action, int) {
  const std::string name = key_name(key, scancode);
  if (WindowInput *const input = input_of(window); input && !name.empty())
    input->key(name, action != GLFW_RELEASE);
}

void on_text(GLFWwindow *window, unsigned int point) {
  if (WindowInput *const input = input_of(window))
    input->text(utf8(point));
}

// GLFW places the pointer in screen units, which a scaled display maps to more pixels;
// the frame block's cursor counts pixels.
void on_pointer(GLFWwindow *window, double x, double y) {
  WindowInput *const input = input_of(window);
  if (!input)
    return;
  int width = 0;
  int height = 0;
  int pixels_x = 0;
  int pixels_y = 0;
  glfwGetWindowSize(window, &width, &height);
  glfwGetFramebufferSize(window, &pixels_x, &pixels_y);
  input->pointer(static_cast<float>(width == 0 ? x : x * pixels_x / width),
                 static_cast<float>(height == 0 ? y : y * pixels_y / height));
}

void on_button(GLFWwindow *window, int button, int action, int) {
  WindowInput *const input = input_of(window);
  if (input && button >= 0 && button < static_cast<int>(buttons.size()))
    input->button(buttons[static_cast<std::size_t>(button)], action == GLFW_PRESS);
}

void on_wheel(GLFWwindow *window, double x, double y) {
  if (WindowInput *const input = input_of(window))
    input->wheel(static_cast<float>(x), static_cast<float>(y));
}

void on_focus(GLFWwindow *window, int focused) {
  if (WindowInput *const input = input_of(window))
    input->focus(focused == GLFW_TRUE);
}

} // namespace

// A name of the table, or one printable character, as a layout's key prints it.
bool key_named(std::string_view name) {
  return std::ranges::find(named_keys, name, &decltype(named_keys)::value_type::second) !=
             named_keys.end() ||
         std::ranges::find(function_keys, name) != function_keys.end() ||
         (!name.empty() && static_cast<unsigned char>(name.front()) > ' ' &&
          name.size() == utf8_length(static_cast<unsigned char>(name.front())));
}

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

// MSVC's typeid already spells a type as code does.
std::string type_name(const char *name) {
  return name;
}

// A pipe, or a file, which never makes a read wait. A console types nothing yet: reading
// one without waiting takes its input events, which no Windows user has needed so far.
std::optional<std::string> Terminal::input() {
  const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  const DWORD type = GetFileType(in);
  if (type == FILE_TYPE_CHAR)
    return std::string();
  DWORD available = static_cast<DWORD>(input_chunk);
  if (type == FILE_TYPE_PIPE &&
      !PeekNamedPipe(in, nullptr, 0, nullptr, &available, nullptr))
    return std::nullopt; // the writer closed it
  available = std::min(available, static_cast<DWORD>(input_per_call));
  std::string text(available, '\0');
  DWORD got = 0;
  if (available != 0 &&
      (!ReadFile(in, text.data(), available, &got, nullptr) || got == 0))
    return std::nullopt;
  text.resize(got);
  return text;
}

bool Terminal::typed() {
  return GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR;
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

// GCC and Clang name a type as the linker does, which the C++ ABI spells out again.
std::string type_name(const char *name) {
  int status = 0;
  const std::unique_ptr<char, decltype(&std::free)> spelled(
      abi::__cxa_demangle(name, nullptr, nullptr, &status), &std::free);
  return status == 0 ? spelled.get() : name;
}

// A read after poll found input never waits; one that finds the end returns nothing.
std::optional<std::string> Terminal::input() {
  std::string text;
  std::array<char, input_chunk> chunk{};
  pollfd in{.fd = STDIN_FILENO, .events = POLLIN};
  while (text.size() < input_per_call && poll(&in, 1, 0) > 0) {
    const ssize_t got = read(STDIN_FILENO, chunk.data(), chunk.size());
    if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN))
      return text.empty() ? std::nullopt : std::optional(std::move(text));
    if (got > 0)
      text.append(chunk.data(), static_cast<std::size_t>(got));
  }
  return text;
}

bool Terminal::typed() {
  return isatty(STDIN_FILENO) == 1;
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

void Files::save(const std::filesystem::path &file, std::string_view text) {
  std::error_code failed;
  // A new view's folder exists once its first save writes it.
  if (file.has_parent_path())
    std::filesystem::create_directories(file.parent_path(), failed);
  std::filesystem::path temp = file;
  temp += ".tmp";
  std::ofstream out(temp, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  if (out && !failed)
    std::filesystem::rename(temp, file, failed);
  if (!out || failed) {
    std::error_code gone; // the temp file may never have been made
    std::filesystem::remove(temp, gone);
    throw std::runtime_error(std::format(
        "{}: cannot be written{}", file.string(), failed ? ": " + failed.message() : ""));
  }
}

// A folder another file still holds, or one in use, stays.
void Files::remove(const std::filesystem::path &file) {
  std::error_code failed;
  if (!std::filesystem::remove(file, failed))
    throw std::runtime_error(std::format("{}: cannot be deleted: {}",
                                         file.string(),
                                         failed ? failed.message() : "it is not there"));
  if (std::filesystem::is_empty(file.parent_path(), failed) && !failed)
    std::filesystem::remove(file.parent_path(), failed);
}

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
  glfwSetKeyCallback(_window, on_key);
  glfwSetCharCallback(_window, on_text);
  glfwSetCursorPosCallback(_window, on_pointer);
  glfwSetMouseButtonCallback(_window, on_button);
  glfwSetScrollCallback(_window, on_wheel);
  glfwSetWindowFocusCallback(_window, on_focus);
}

Window::~Window() {
  glfwDestroyWindow(_window);
  glfwTerminate();
}

bool Window::poll(WindowInput &input) const {
  glfwSetWindowUserPointer(_window, &input);
  glfwPollEvents();
  glfwSetWindowUserPointer(_window, nullptr);
  return !glfwWindowShouldClose(_window);
}

VkExtent2D Window::size() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(_window, &width, &height);
  return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

VkSurfaceKHR Window::surface(VkInstance instance) const {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(instance, _window, nullptr, &surface) != VK_SUCCESS)
    glfw_failed("glfwCreateWindowSurface");
  return surface;
}

} // namespace VP
