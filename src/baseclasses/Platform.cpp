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
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace VP {

namespace {

constexpr std::size_t input_chunk = 4096; // bytes of standard input one read takes
constexpr std::size_t common_columns = 80; // as a terminal starts
// At most this much input a frame, so input that never pauses cannot hold a frame.
constexpr std::size_t input_per_call = 16 * input_chunk;
// What the desktop knows the window by, as Wayland's app id and X11's class, so a dock
// finds it again after a click minimized it; without it GLFW sends none on Wayland.
constexpr const char *app_id = "vulpen";

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

// A name of the table, or one printable character, as a layout's key prints it without
// shift, as glfwGetKeyName gives it, so a letter is lower case.
bool key_named(std::string_view name) {
  return std::ranges::find(named_keys, name, &decltype(named_keys)::value_type::second) !=
             named_keys.end() ||
         std::ranges::find(function_keys, name) != function_keys.end() ||
         (!name.empty() && static_cast<unsigned char>(name.front()) > ' ' &&
          (name.front() < 'A' || name.front() > 'Z') &&
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

// A scanner, as Defender, opens a file just written, and a reader may hold the file it
// replaces; either keeps it from being renamed over the other for a moment, so a save
// tries again a while before it fails.
constexpr DWORD rename_retry = 10; // milliseconds between tries
constexpr int rename_tries = 50;   // half a second in all

void rename_over(const std::filesystem::path &from,
                 const std::filesystem::path &to,
                 std::error_code &failed) {
  for (int tried = 1;; ++tried) {
    std::filesystem::rename(from, to, failed);
    const bool held = failed.category() == std::system_category() &&
                      (failed.value() == ERROR_SHARING_VIOLATION ||
                       failed.value() == ERROR_ACCESS_DENIED);
    if (!held || tried == rename_tries)
      return;
    Sleep(rename_retry);
  }
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

// MSVC's typeid names a class with its key, as struct Count, the unnamed namespace as
// `anonymous namespace', and packs a template's arguments; spelt as GCC and Clang spell
// them, a type reads alike on every system, and as code writes it.
std::string type_name(const char *name) {
  std::string text = name;
  for (const std::string_view key : {"struct ", "class ", "union ", "enum "})
    for (std::size_t at = text.find(key); at != std::string::npos;
         at = text.find(key, at))
      if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0 ||
                     text[at - 1] == '_'))
        at += key.size();
      else
        text.erase(at, key.size());
  constexpr std::string_view unnamed = "`anonymous namespace'";
  for (std::size_t at = text.find(unnamed); at != std::string::npos;
       at = text.find(unnamed, at))
    text.replace(at, unnamed.size(), "(anonymous namespace)");
  for (std::size_t at = text.find(','); at != std::string::npos;
       at = text.find(',', at + 1))
    if (text[at + 1] != ' ')
      text.insert(at + 1, " ");
  return text;
}

namespace {

constexpr DWORD console_records = 64; // input events a read takes at most

// The characters typed at a console so far, as UTF-8, without waiting: a key pressed, as
// often as it repeats, among them the escape sequence a key past text sends in VT input
// mode, as a Unix terminal does; nothing for a key let go, the mouse or the window.
std::string console_input(HANDLE in) {
  std::wstring typed;
  std::array<INPUT_RECORD, console_records> records{};
  DWORD waiting = 0;
  while (typed.size() < input_per_call && GetNumberOfConsoleInputEvents(in, &waiting) &&
         waiting > 0) {
    DWORD got = 0;
    if (!ReadConsoleInputW(in, records.data(), std::min(waiting, console_records), &got))
      break;
    for (DWORD at = 0; at < got; ++at) {
      if (records[at].EventType != KEY_EVENT)
        continue;
      const KEY_EVENT_RECORD &key = records[at].Event.KeyEvent;
      if (key.bKeyDown && key.uChar.UnicodeChar != 0)
        typed.append(key.wRepeatCount, key.uChar.UnicodeChar);
    }
  }
  if (typed.empty())
    return {};
  const auto units = static_cast<int>(typed.size());
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, typed.data(), units, nullptr, 0, nullptr, nullptr);
  std::string text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, typed.data(), units, text.data(), size, nullptr, nullptr);
  return text;
}

} // namespace

// A console's keys as they come, or a pipe or a file, which never makes a read wait.
std::optional<std::string> Terminal::input() {
  const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  if (typed())
    return console_input(in);
  DWORD available = static_cast<DWORD>(input_chunk);
  if (GetFileType(in) == FILE_TYPE_PIPE &&
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

// A console, as no pipe or file has a console mode, nor NUL, which reads as ended.
bool Terminal::typed() {
  DWORD mode = 0;
  return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

std::size_t Terminal::columns() {
  CONSOLE_SCREEN_BUFFER_INFO shown{};
  return GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &shown)
             ? static_cast<std::size_t>(shown.srWindow.Right - shown.srWindow.Left + 1)
             : common_columns;
}

struct RawTerminal::Saved {
  DWORD input = 0;
  DWORD output = 0;
  bool shown = false; // standard output is a console, whose mode changed too
};

// Without line input, echo and processed input, which makes control with c a signal; with
// VT input, so a key past text sends the escape sequence a Unix terminal does. A console
// that shows the line typed takes the sequences that draw it, with colors or without.
RawTerminal::RawTerminal() : _saved(std::make_unique<Saved>()) {
  const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  constexpr DWORD cooked = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
  if (!GetConsoleMode(in, &_saved->input))
    throw last_error("GetConsoleMode");
  if (!SetConsoleMode(in, (_saved->input & ~cooked) | ENABLE_VIRTUAL_TERMINAL_INPUT))
    throw last_error("SetConsoleMode");
  _saved->shown =
      GetConsoleMode(out, &_saved->output) &&
      SetConsoleMode(out, _saved->output | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

RawTerminal::~RawTerminal() {
  SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), _saved->input);
  if (_saved->shown)
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), _saved->output);
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

// where finds the program as cmd does, and start runs it on its own and returns at once;
// start's first quoted word is a title, so an empty one comes first.
void Shell::start(const std::string &program, std::span<const std::string> arguments) {
  std::string line = std::format("\"{}\"", program);
  for (const std::string &argument : arguments)
    line.append(std::format(" \"{}\"", argument));
  if (run(std::format("where /q \"{}\" && start \"\" {}", program, line)) != 0)
    throw std::runtime_error(std::format("{} is not on the PATH", program));
}

namespace {

constexpr DWORD lock_retry = 100; // milliseconds between tries of a lock another holds

// A word as a program's command line reads it back (CommandLineToArgvW): in quotes when
// it holds a blank or a quote, each quote in it escaped, and the backslashes before a
// quote doubled.
std::wstring quoted(const std::wstring &word) {
  if (!word.empty() && word.find_first_of(L" \t\"") == std::wstring::npos)
    return word;
  std::wstring text = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t letter : word) {
    if (letter == L'\\') {
      ++backslashes;
      continue;
    }
    text.append(letter == L'"' ? 2 * backslashes + 1 : backslashes, L'\\');
    text.push_back(letter);
    backslashes = 0;
  }
  text.append(2 * backslashes, L'\\');
  return text + L'"';
}

} // namespace

// The new process takes this one's standard streams, a pipe's too, as handles it inherits.
void Shell::replace(const std::filesystem::path &program,
                    std::span<const std::string> arguments) {
  std::wstring line = quoted(program.wstring());
  for (const std::string &argument : arguments)
    line.append(L" ").append(quoted(std::filesystem::path(argument).wstring()));
  const std::array streams{GetStdHandle(STD_INPUT_HANDLE),
                           GetStdHandle(STD_OUTPUT_HANDLE),
                           GetStdHandle(STD_ERROR_HANDLE)};
  for (const HANDLE stream : streams)
    SetHandleInformation(stream, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW),
                       .dwFlags = STARTF_USESTDHANDLES,
                       .hStdInput = streams[0],
                       .hStdOutput = streams[1],
                       .hStdError = streams[2]};
  PROCESS_INFORMATION started{};
  std::fflush(nullptr);
  if (!CreateProcessW(program.c_str(),
                      line.data(),
                      nullptr,
                      nullptr,
                      TRUE,
                      0,
                      nullptr,
                      nullptr,
                      &startup,
                      &started))
    throw last_error("CreateProcessW " + program.string());
  CloseHandle(started.hThread);
  WaitForSingleObject(started.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(started.hProcess, &code);
  CloseHandle(started.hProcess);
  ExitProcess(code);
}

struct FileLock::Held {
  HANDLE file = INVALID_HANDLE_VALUE;
};

// Opened with no sharing, as run.ps1 opens it, so the next to open it waits until this
// closes it.
FileLock::FileLock(const std::filesystem::path &file) : _held(std::make_unique<Held>()) {
  for (;;) {
    _held->file = CreateFileW(file.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (_held->file != INVALID_HANDLE_VALUE)
      return;
    if (GetLastError() != ERROR_SHARING_VIOLATION)
      throw last_error("CreateFileW " + file.string());
    Sleep(lock_retry);
  }
}

FileLock::~FileLock() {
  CloseHandle(_held->file);
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

namespace {

// A lock file's: read and write for its owner, read for everyone else.
constexpr mode_t lock_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

// Whoever holds either file, Linux renames one over the other.
void rename_over(const std::filesystem::path &from,
                 const std::filesystem::path &to,
                 std::error_code &failed) {
  std::filesystem::rename(from, to, failed);
}

// Whether standard input is a terminal another job holds, as a run started in the
// background finds it: a read would stop the run (SIGTTIN), and what is typed there is
// that job's.
bool held_elsewhere() {
  return isatty(STDIN_FILENO) == 1 && tcgetpgrp(STDIN_FILENO) != getpgrp();
}

} // namespace

// A read after poll found input never waits; one that finds the end returns nothing.
std::optional<std::string> Terminal::input() {
  if (held_elsewhere())
    return std::string();
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
  return isatty(STDIN_FILENO) == 1 && !held_elsewhere();
}

std::size_t Terminal::columns() {
  winsize size{};
  return ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 ? size.ws_col
                                                                         : common_columns;
}

struct RawTerminal::Saved {
  termios mode;
};

// A read takes what came without waiting, as poll asks anyway.
RawTerminal::RawTerminal() {
  termios mode{};
  if (tcgetattr(STDIN_FILENO, &mode) != 0)
    throw std::system_error(errno, std::generic_category(), "tcgetattr");
  _saved = std::make_unique<Saved>(Saved{mode});
  mode.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
  mode.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
  mode.c_cc[VMIN] = 0;
  mode.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) != 0)
    throw std::system_error(errno, std::generic_category(), "tcsetattr");
}

RawTerminal::~RawTerminal() {
  tcsetattr(STDIN_FILENO, TCSANOW, &_saved->mode);
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

namespace {

// In single quotes, so a blank or a quote in a word stays in it.
std::string single_quoted(std::string_view word) {
  std::string text = "'";
  for (const char letter : word)
    text.append(letter == '\'' ? std::string_view("'\\''")
                               : std::string_view(&letter, 1));
  return text + "'";
}

} // namespace

// command -v finds the program as the shell does, which then starts it in the background
// and ends at once, so the program is no child of vulpen's: nothing waits for it, and
// none is left to wait for.
void Shell::start(const std::string &program, std::span<const std::string> arguments) {
  std::string line = single_quoted(program);
  for (const std::string &argument : arguments)
    line.append(" ").append(single_quoted(argument));
  if (std::system(std::format("command -v {} >/dev/null || exit 1; {} >/dev/null 2>&1 &",
                              single_quoted(program),
                              line)
                      .c_str()) != 0)
    throw std::runtime_error(std::format("{} is not on the PATH", program));
}

// The streams flush first, as nothing else ends this process to flush them.
void Shell::replace(const std::filesystem::path &program,
                    std::span<const std::string> arguments) {
  std::vector<std::string> words{program.string()};
  words.insert(words.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (std::string &word : words)
    argv.push_back(word.data());
  argv.push_back(nullptr);
  std::fflush(nullptr);
  execv(words.front().c_str(), argv.data());
  throw std::system_error(errno, std::generic_category(), "execv " + words.front());
}

struct FileLock::Held {
  int file = -1;
};

// flock, as run.sh's flock command takes it, so a second lock waits even in one process:
// it locks an open file, not the process. Closed on exec, so neither a program the shell
// runs nor the vulpen a restart runs holds it.
FileLock::FileLock(const std::filesystem::path &file) : _held(std::make_unique<Held>()) {
  _held->file = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, lock_mode);
  if (_held->file < 0)
    throw std::system_error(errno, std::generic_category(), "open " + file.string());
  while (flock(_held->file, LOCK_EX) != 0)
    if (errno != EINTR) {
      const std::system_error failure(
          errno, std::generic_category(), "flock " + file.string());
      close(_held->file);
      throw failure;
    }
}

FileLock::~FileLock() {
  close(_held->file);
}

#endif

// The row starts anew, so the cursor goes back to its start and forward to the caret.
void Terminal::draw(std::string_view text, std::size_t caret) {
  std::fwrite(clear_row.data(), 1, clear_row.size(), stdout);
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\r', stdout);
  if (caret > 0)
    std::fprintf(stdout, "\033[%zuC", caret);
  std::fflush(stdout);
}

std::string Files::text(const std::filesystem::path &file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

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
    rename_over(temp, file, failed);
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

// A temp a killed copy left goes first, so it never blocks the next. A copy inside what
// it copies would copy itself without end.
void Files::copy(const std::filesystem::path &from, const std::filesystem::path &to) {
  std::error_code failed;
  if (std::filesystem::exists(to, failed))
    throw std::runtime_error(std::format("{} exists already", to.string()));
  const std::filesystem::path inside =
      to.lexically_normal().lexically_relative(from.lexically_normal());
  if (!inside.empty() && *inside.begin() != "..")
    throw std::runtime_error(std::format(
        "{} is inside {}, so it cannot hold a copy of it", to.string(), from.string()));
  std::filesystem::path temp = to;
  temp += ".tmp";
  std::filesystem::remove_all(temp, failed);
  if (!failed && to.has_parent_path())
    std::filesystem::create_directories(to.parent_path(), failed);
  if (!failed)
    std::filesystem::copy(from, temp, std::filesystem::copy_options::recursive, failed);
  if (!failed)
    rename_over(temp, to, failed);
  if (failed) {
    std::error_code gone; // the temp may never have been made
    std::filesystem::remove_all(temp, gone);
    throw std::runtime_error(std::format(
        "{}: cannot be copied to {}: {}", from.string(), to.string(), failed.message()));
  }
}

void Files::rename(const std::filesystem::path &from, const std::filesystem::path &to) {
  std::error_code failed;
  if (std::filesystem::exists(to, failed))
    throw std::runtime_error(std::format("{} exists already", to.string()));
  std::filesystem::rename(from, to, failed);
  if (failed)
    throw std::runtime_error(std::format(
        "{}: cannot be renamed to {}: {}", from.string(), to.string(), failed.message()));
}

Library::Library(Library &&other) noexcept
    : _handle(std::exchange(other._handle, nullptr)), _copy(std::move(other._copy)) {}

Library &Library::operator=(Library &&other) noexcept {
  std::swap(_handle, other._handle);
  std::swap(_copy, other._copy);
  return *this;
}

Desktop::Desktop() {
  // GLFW then finds Vulkan through the loader vulpen links, not a second one.
  glfwInitVulkanLoader(vkGetInstanceProcAddr);
  if (!glfwInit())
    glfw_failed("glfwInit");
}

Desktop::~Desktop() {
  glfwTerminate();
}

void Desktop::poll() const {
  glfwPollEvents();
}

Window::Window(const Desktop &, const std::string &title, VkExtent2D size) {
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  // It fills the screen as it opens, as an editor does; size is what it is restored to.
  glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, app_id);
  glfwWindowHintString(GLFW_X11_CLASS_NAME, app_id);
  glfwWindowHintString(GLFW_X11_INSTANCE_NAME, app_id);
  _window = glfwCreateWindow(static_cast<int>(size.width),
                             static_cast<int>(size.height),
                             title.c_str(),
                             nullptr,
                             nullptr);
  if (!_window)
    glfw_failed("glfwCreateWindow");
  glfwSetKeyCallback(_window, on_key);
  glfwSetCharCallback(_window, on_text);
  glfwSetCursorPosCallback(_window, on_pointer);
  glfwSetMouseButtonCallback(_window, on_button);
  glfwSetScrollCallback(_window, on_wheel);
  glfwSetWindowFocusCallback(_window, on_focus);
}

Window::~Window() {
  glfwDestroyWindow(_window);
}

bool Window::poll(WindowInput &input) const {
  glfwSetWindowUserPointer(_window, &input);
  glfwPollEvents();
  glfwSetWindowUserPointer(_window, nullptr);
  const bool asked = glfwWindowShouldClose(_window) == GLFW_TRUE;
  glfwSetWindowShouldClose(_window, GLFW_FALSE);
  return !asked;
}

VkExtent2D Window::size() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(_window, &width, &height);
  return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

std::string Window::clipboard() const {
  const char *const text = glfwGetClipboardString(_window);
  if (!text)
    glfwGetError(nullptr); // an empty clipboard is no failure a later call should report
  return text ? text : "";
}

void Window::copy(const std::string &text) const {
  glfwSetClipboardString(_window, text.c_str());
}

VkSurfaceKHR Window::surface(VkInstance instance) const {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(instance, _window, nullptr, &surface) != VK_SUCCESS)
    glfw_failed("glfwCreateWindowSurface");
  return surface;
}

} // namespace VP
