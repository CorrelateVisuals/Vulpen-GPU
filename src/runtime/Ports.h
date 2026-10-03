#pragma once

#include "runtime/Operator.h"

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

private:
  std::string read(std::string_view file) override;
  void save(std::string_view file, std::string_view text) override;
  std::vector<std::string> list(std::string_view folder) override;
  std::span<const std::string> lines() override;
  bool ended() const override;
  void print(std::string_view text) override;
  void command(Call &call) override;

  std::vector<std::string> _lines; // this frame's
  std::string _partial;            // a line still being typed
  bool _read = false;              // whether this frame read standard input
  bool _ended = false;
};

} // namespace VP
