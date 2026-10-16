#pragma once

#include "runtime/Operator.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

class Commands;
class Log;
class RawTerminal;
class Window;

// The general ports a node reads besides commands (V05): input from the window or the
// input command, files, and the terminal's lines. One owner feeds every node, so each
// frame's events reach them all alike.
class Ports final : public InputPort,
                    public FilePort,
                    public ProcessPort,
                    public TerminalPort,
                    public CommandHandler {
public:
  // log: what clears the line typed at the terminal before it prints.
  explicit Ports(const Log &log);
  // The line typed goes first, so what prints after the run starts on a clean row.
  ~Ports();
  Ports(const Ports &) = delete;
  Ports &operator=(const Ports &) = delete;
  // Before a frame cooks: what came in for the last one is gone, and what came since is
  // this frame's.
  void frame();
  // An event for the next frame, from the window or the input command; the pointer's
  // moves in a row keep only the last.
  void add(Event event);
  // Registers the input command, whose events the log never keeps, as the lead chose on
  // 2026-10-06: a replay rebuilds the graph, not the input that drove it.
  void listen(Commands &commands);
  // The window whose clipboard copy and paste use while it is open; null once it closes.
  void attach(const Window *window);
  // A file a node opens while it binds. One open already is shared, so a rebuild's open
  // reads nothing. Throws naming a file that is there but cannot be read (A02).
  File open(const std::filesystem::path &file);
  // Once a node that opened it is gone.
  void close(File file);

private:
  // A file the nodes opened, shared by every node that did.
  struct Open {
    std::filesystem::path path;
    std::string text;
    std::filesystem::file_time_type written = std::filesystem::file_time_type::min();
    std::uint64_t checked = 0; // the frame it was last compared with the disk
    std::uint32_t nodes = 1;   // that hold it
  };

  Open &opened(File file);
  void refresh(Open &open);
  std::string_view text(File file) override;
  void save(File file, std::string_view text) override;
  std::string read(std::string_view file) override;
  void save(std::string_view file, std::string_view text) override;
  void remove(std::string_view file) override;
  std::vector<std::string> list(std::string_view folder) override;
  void copy(std::string_view from, std::string_view to) override;
  void rename(std::string_view from, std::string_view to) override;
  void save(const View &view, std::string_view file) override;
  void start(std::string_view program, std::span<const std::string> arguments) override;
  View manifest(std::string_view file) override;
  std::span<const Event> events() const override;
  glm::vec2 pointer() const override;
  bool names_key(std::string_view name) const override;
  std::string clipboard() const override;
  void copy(std::string_view text) override;
  bool windowed() const override;
  bool typed() const override;
  std::span<const Event> keys() override;
  std::span<const std::string> lines() override;
  bool ended() const override;
  void print(std::string_view text) override;
  void prompt(std::string_view text) override;
  void line(std::string_view text, std::size_t caret) override;
  std::size_t columns() const override;
  void command(Call &call) override;

  // By id, from a counter, so a handle a bind no longer gave matches no file.
  std::map<std::uint32_t, Open> _open;
  std::uint32_t _opened = 0;
  std::uint64_t _frames = 0;
  const Log &_log;
  std::vector<std::string> _lines; // this frame's
  std::string _partial;            // a line still being typed
  bool _read = false;              // whether this frame read standard input
  bool _ended = false;
  std::unique_ptr<RawTerminal> _raw; // once a node asks for keys, while a person types
  std::vector<Event> _keys;          // this frame's
  std::string _bytes;                // a key's first bytes, which wait for the rest
  bool _keyed = false;               // whether this frame read keys
  std::vector<Event> _events;  // this frame's
  std::vector<Event> _pending; // the next frame's
  glm::vec2 _pointer{};
  const Window *_window = nullptr; // borrowed from the runtime, which owns it (A01)
  std::string _clipboard;          // what was copied last, for a run with no window
  // The input command's forms, as the window's events come in.
  struct Input {
    Command key_down, key_up, text, pointer, button_down, button_up, wheel, focus_on,
        focus_off;
  } _input;
};

} // namespace VP
