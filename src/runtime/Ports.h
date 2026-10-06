#pragma once

#include "runtime/Operator.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

class Commands;

// The general ports a node reads besides commands (V05): input from the window or the
// input command, files, and the terminal's lines. One owner feeds every node, so each
// frame's events reach them all alike.
class Ports final : public InputPort,
                    public FilePort,
                    public TerminalPort,
                    public CommandHandler {
public:
  // Before a frame cooks: what came in for the last one is gone, and what came since is
  // this frame's.
  void frame();
  // An event for the next frame, from the window or the input command; the pointer's
  // moves in a row keep only the last.
  void add(Event event);
  // Registers the input command, whose events the log never keeps, as the lead chose on
  // 2026-10-06: a replay rebuilds the graph, not the input that drove it.
  void listen(Commands &commands);
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
  View manifest(std::string_view file) override;
  std::span<const Event> events() const override;
  glm::vec2 pointer() const override;
  std::span<const std::string> lines() override;
  bool ended() const override;
  void print(std::string_view text) override;
  void prompt(std::string_view text) override;
  void command(Call &call) override;

  // By id, from a counter, so a handle a bind no longer gave matches no file.
  std::map<std::uint32_t, Open> _open;
  std::uint32_t _opened = 0;
  std::uint64_t _frames = 0;
  std::vector<std::string> _lines; // this frame's
  std::string _partial;            // a line still being typed
  bool _read = false;              // whether this frame read standard input
  bool _ended = false;
  std::vector<Event> _events;  // this frame's
  std::vector<Event> _pending; // the next frame's
  glm::vec2 _pointer{};
  // The input command's forms, as the window's events come in.
  struct Input {
    Command key_down, key_up, text, pointer, button_down, button_up, wheel, focus_on,
        focus_off;
  } _input;
};

} // namespace VP
