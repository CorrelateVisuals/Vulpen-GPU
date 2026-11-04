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
struct Recipes;

// The general ports a node reads besides commands (V05): input from the window or the
// input command, files, and the terminal's lines. One owner feeds every node, so each
// frame's events reach them all alike.
class Ports final : public InputPort,
                    public FilePort,
                    public ProcessPort,
                    public TerminalPort,
                    public CommandHandler {
public:
  // log: what clears the line typed at the terminal before it prints. recipes: what a
  // manifest read here unfolds its recipes from, borrowed (A01).
  Ports(const Log &log, Recipes &recipes);
  // The line typed goes first, so what prints after the run starts on a clean row.
  ~Ports();
  Ports(const Ports &) = delete;
  Ports &operator=(const Ports &) = delete;
  // Before a frame cooks: what came in for the last one is gone, and what came since is
  // this frame's.
  void frame();
  // The input as a node of a view reads it this frame: all of it, unless a node outside
  // the one it is in held the pointer the frame before (Cook::hold_pointer).
  InputPort &input(std::string_view view, std::string_view node);
  void hold_pointer(std::string_view view, std::string_view node);
  // An event for the next frame, from the window or the input command; the pointer's
  // moves in a row keep only the last.
  void add(Event event);
  // Registers the input command, whose events the log never keeps: a replay rebuilds the
  // graph, not the input that drove it.
  void listen(Commands &commands);
  // The window whose clipboard copy and paste use while it is open; null once it closes.
  void attach(const Window *window);
  // Whether a file or a folder was made, removed, moved or copied through the port since
  // the last ask, so the views list their nodes' files anew; a hidden one, as the parts
  // keep in .vulpen/, never counts, as no node holds it.
  bool relisted();
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

  // What a node outside a hold reads of the input, in Ports.cpp.
  class Unheld;
  // The nodes a hold keeps the pointer for: those of a view whose names start with
  // inside. Swapped a frame, so their names never allocate again (CPP10).
  struct Holder {
    std::string view;
    std::string inside;
    bool holds = false;
  };

  Open &opened(File file);
  void refresh(Open &open);
  // Marks a change a node's folder may hold, unless the path is hidden.
  void listing_changed(const std::filesystem::path &path);
  std::string_view text(File file) override;
  void save(File file, std::string_view text) override;
  std::string read(std::string_view file) override;
  void save(std::string_view file, std::string_view text) override;
  void remove(std::string_view file) override;
  std::vector<std::string> list(std::string_view folder) override;
  void copy(std::string_view from, std::string_view to) override;
  void rename(std::string_view from, std::string_view to) override;
  void make(std::string_view folder) override;
  std::span<const Moved> moved() const override;
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
  Recipes &_recipes;
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
  Holder _holding;             // asked for this frame
  Holder _held;                // asked for the frame before, which this frame keeps
  std::vector<Event> _unheld;  // while it does, this frame's but presses and the wheel
  std::unique_ptr<Unheld> _unheld_input;
  glm::vec2 _pointer{};
  const Window *_window = nullptr; // borrowed from the runtime, which owns it (A01)
  std::string _clipboard;          // what was copied last, for a run with no window
  // The renames and removes of the frame before, then this frame's: how many are the
  // frame before's.
  std::vector<Moved> _moved;
  std::size_t _moved_before = 0;
  bool _relisted = false; // since relisted last asked
  // The input command's forms, as the window's events come in.
  struct Input {
    Command key_down, key_up, text, pointer, button_down, button_up, wheel, focus_on,
        focus_off;
  } _input;
};

} // namespace VP
