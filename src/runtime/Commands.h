#pragma once

#include "runtime/Operator.h"

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace VP {

// Finds the view a command addresses by name (`triangle: node add …`). The views' owner
// implements it, so the port reaches a view without including its owner.
class ViewLookup {
public:
  // An empty name finds the view vulpen started with; null when no view has the name.
  virtual const View *find(std::string_view name) = 0;
  // Swaps in a changed copy of the view, with a schedule built on the path a live swap
  // takes, so it keeps what the change left alone. The running schedule points into the
  // old view until then, so a change never edits a view in place.
  virtual void replace(std::string_view name, View view) = 0;

protected:
  ~ViewLookup() = default;
};

// Every change to a view is a command through this one port, so the CLI, a GUI, a
// script and an agent act alike, and replaying the log rebuilds the session. A command
// registers with its usage, whose placeholders give its completion, and its help, or
// not at all (RV04).
class Commands final : public CommandPort, public CommandHandler {
public:
  explicit Commands(const Log &log);
  ~Commands();
  Commands(const Commands &) = delete;
  Commands &operator=(const Commands &) = delete;

  // usage: the command's words as a person types them. Throws naming the mistake, so a
  // command without its usage and help never registers (A02).
  Command add(std::string_view usage, std::string_view help, CommandHandler &handler);
  // Throws naming the mistake: a command nobody registered, or words its usage does not
  // take.
  void run(std::string_view line);
  // Runs the file's lines in turn until one quits. Throws naming the file and line of
  // the first that fails, and runs none after it, so a script never goes on from a
  // state it did not expect.
  void source(const std::filesystem::path &file);
  // Whether quit ran, so the run ends before its next frame.
  bool quitting() const;

private:
  struct Spec;

  void command(Call &call) override;
  const Spec *match(std::span<const std::string_view> words) const;

  const Log &_log;
  std::vector<Spec> _specs;
  const Command _quit;
  bool _quitting = false;
};

// The session as groups: each line keeps the primitive commands it expanded to, so a
// replay needs no recipe (V08) and undo removes one group.
class CommandLog {};

} // namespace VP
