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
class Speakers;
class Window;
struct Recipes;

// The general ports a node reads besides commands (V05): input from the window or the
// input command, files, and the terminal's lines. One owner feeds every node, so each
// frame's events reach them all alike.
class Ports final : public InputPort,
                    public FilePort,
                    public ProcessPort,
                    public TerminalPort,
                    public SoundPort,
                    public CommandHandler {
public:
  // log: what clears the line typed at the terminal before it prints. recipes: what a
  // manifest read here unfolds its recipes from, borrowed (A01). mirror: the build tree's
  // views, where what the build made of a folder a remove or rename takes goes with it.
  Ports(const Log &log, Recipes &recipes, std::filesystem::path mirror);
  // The line typed goes first, so what prints after the run starts on a clean row.
  ~Ports();
  Ports(const Ports &) = delete;
  Ports &operator=(const Ports &) = delete;
  // Before a frame cooks: what came in for the last one is gone, and what came since is
  // this frame's.
  void frame();
  // Where a GUI shows a view's whole window, in the window's pixels, its top left and how
  // far it reaches, and how large that window is (Cook::hand_pointer).
  struct Shown {
    glm::vec2 at{};
    glm::vec2 size{};
    glm::vec2 window{};
  };

  // The input as a node of a view reads it this frame: all of it, unless a node outside
  // the one it is in held the pointer the frame before (Cook::hold_pointer); then the
  // pointer's moves but nothing pressed, let go or turned, and for a view drawn into an
  // image, no wheel, press or move of it at all, as the GUI holding it shows the view
  // somewhere else than under the pointer. A view handed the pointer reads it whole,
  // over its own window, as the GUI shows it. windowed: whether the view draws into the
  // window, rather than into an image or nowhere.
  InputPort &input(std::string_view view, std::string_view node, bool windowed);
  void hold_pointer(std::string_view view, std::string_view node);
  // As hold_pointer, the nodes of the view named to taking the presses too, over the
  // window of theirs a GUI shows where shown says (Cook::hand_pointer).
  void hand_pointer(std::string_view to,
                    std::string_view view,
                    std::string_view node,
                    Shown shown);
  // A node of a view reads nothing at an input this frame (Cook::unread). Until the next
  // frame, whether a node said so as the frame before cooked, which decides the views
  // that rest.
  void unread(std::string_view view, std::string_view node, std::string_view port);
  bool was_unread(std::string_view view,
                  std::string_view node,
                  std::string_view port) const;
  // An event for the next frame, from the window or the input command; the pointer's
  // moves in a row keep only the last.
  void add(Event event);
  // Registers the input command, whose events the log never keeps: a replay rebuilds the
  // graph, not the input that drove it.
  void listen(Commands &commands);
  // The window whose clipboard copy and paste use while it is open; null once it closes.
  void attach(const Window *window);
  // Before a frame cooks: the frame before's sound becomes what played, and the frame's
  // starts silent, frames long. After it cooked: the frame's goes to the speakers, which
  // open as a node first adds to it while a window is open.
  void start_sound(std::size_t frames);
  void play_sound();
  std::size_t frames() const override;
  void add(std::span<const float> samples) override;
  std::span<const float> played() const override;
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

  // What a node reads of the input but all of it as the window gave it: outside a hold,
  // of a view drawn into an image outside it, or of a view handed the pointer, in
  // Ports.cpp.
  class Filtered;
  // The nodes a hold keeps the pointer for: those of a view whose names start with
  // inside, and every node of the view a hand names, as handed, over its window where
  // shown says. Swapped a frame, so their names never allocate again (CPP10).
  struct Holder {
    std::string view;
    std::string inside;
    std::string handed;
    Shown shown;
    bool holds = false;
  };

  Open &opened(File file);
  void refresh(Open &open);
  // Marks a change a node's folder may hold, unless the path is hidden.
  void listing_changed(const std::filesystem::path &path);
  void unbuild(const std::filesystem::path &view, const std::filesystem::path &folder) const;
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
  const std::filesystem::path _mirror;
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
  // The inputs read nothing at as the frame cooks: the first count are this frame's and
  // the rest are written over next, so a frame allocates nothing once they grew (CPP10).
  struct Unread {
    std::string view;
    std::string node;
    std::string port;
  };
  std::vector<Unread> _unread;
  std::size_t _unread_count = 0;
  Holder _holding;             // asked for this frame
  Holder _held;                // asked for the frame before, which this frame keeps
  std::vector<Event> _unheld;  // while it does, this frame's but presses and the wheel
  std::vector<Event> _unpointed; // and but its moves, the keys pressed and text too
  std::vector<Event> _handed;    // this frame's, the pointer over the window handed
  std::unique_ptr<Filtered> _unheld_input;
  std::unique_ptr<Filtered> _unpointed_input;
  std::unique_ptr<Filtered> _handed_input;
  glm::vec2 _pointer{};
  const Window *_window = nullptr; // borrowed from the runtime, which owns it (A01)
  std::string _clipboard;          // what was copied last, for a run with no window
  // The renames and removes of the frame before, then this frame's: how many are the
  // frame before's.
  std::vector<Moved> _moved;
  std::size_t _moved_before = 0;
  bool _relisted = false; // since relisted last asked
  void window(Call &call);

  // The input command's forms, as the window's events come in.
  struct Input {
    Command key_down, key_up, text, pointer, button_down, button_up, wheel, focus_on,
        focus_off, axis;
  } _input;
  // The window command's forms, and what they set, which a window opened later takes
  // too, so a run with none takes them as one with a window does (V07).
  struct Windowing {
    Command title, fullscreen_on, fullscreen_off, pointer_lock, pointer_free;
  } _windowing;
  std::vector<float> _sound;  // the frame's, as the nodes add to it
  std::vector<float> _played; // the frame before's
  std::unique_ptr<Speakers> _speakers;
  bool _deaf = false; // the speakers would not open, so they are not tried again
  std::string _title; // none for the one the view's name gives
  bool _fills = false;
  bool _locked = false;
};

} // namespace VP
