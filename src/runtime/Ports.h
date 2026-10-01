#pragma once

#include "runtime/Operator.h"

namespace VP {

// The general ports a node reads besides commands (V05): input from the window or the
// input command, files, and the terminal's lines. One owner feeds every node, so each
// frame's events reach them all alike.
class Ports final : public InputPort,
                    public FilePort,
                    public TerminalPort,
                    public CommandHandler {
  void command(Call &call) override;
};

} // namespace VP
