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

// The general ports a node reads besides commands (V05): input from the window or the
// input command, files, and the terminal's lines. One owner feeds every node, so each
// frame's events reach them all alike.
class Ports final : public InputPort,
                    public FilePort,
                    public TerminalPort,
                    public CommandHandler {
public:
  // Before a frame cooks: what came in for the last one is gone.
  void frame();
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
  std::vector<std::string> list(std::string_view folder) override;
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
};

} // namespace VP
