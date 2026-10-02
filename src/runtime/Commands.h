#pragma once

#include "runtime/Operator.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

// Finds the view a command addresses by name (`triangle: node add …`). The views' owner
// implements it, so the port reaches a view without including its owner.
class ViewLookup {
public:
  // An empty name finds the view vulpen started with, as the changes so far left it;
  // null when no view has the name.
  virtual const View *find(std::string_view name) = 0;
  // Takes a changed copy of the view. The schedule is rebuilt from it before the next
  // frame, once for every change since the last frame, on the path a live swap takes, so
  // it keeps what the changes left alone and checks only the view they leave. The
  // running schedule points into the old view until then, so a change never edits a view
  // in place.
  virtual void replace(std::string_view name, View view) = 0;

protected:
  ~ViewLookup() = default;
};

// Whether the log keeps a command's runs. A primitive edits a view or saves it, so
// replaying the primitives rebuilds the graph and the files saved from it (V08). Any
// other command only reads, or runs others, whose primitives the log keeps in its place.
enum class Primitive : bool { no, yes };

// The session as groups: each line from outside the port keeps the primitives it ran,
// so a replay needs no recipe (V08) and undo removes one group.
class CommandLog {
public:
  // A line from outside the port, typed or read from a file: the primitives it runs
  // join one group.
  void open();
  void keep(std::string_view primitive);
  // Every primitive kept, a line each, in order: what source replays.
  std::string text() const;

private:
  std::vector<std::vector<std::string>> _groups;
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
  Command add(std::string_view usage,
              std::string_view help,
              CommandHandler &handler,
              Primitive primitive = Primitive::no);
  // Before the command's handler goes, so no run reaches it after.
  void remove(Command command);
  // A line from outside the port, in a group of its own; returns what its command
  // answers. Throws naming the mistake: a command nobody registered, or words its usage
  // does not take.
  std::string run(std::string_view line);
  std::string send(std::string_view line) override;
  // Runs a file's lines in turn until one quits, each in a group of its own, so undo
  // after a replay steps back one command at a time, and prints what each answers.
  // Throws naming the file and line of the first that fails, and runs none after it, so
  // a script never goes on from a state it did not expect.
  void source(const std::filesystem::path &file);
  // Whether quit ran, so the run ends before its next frame.
  bool quitting() const;
  // The file and line running, as `script.txt:7`; empty for a line typed or sent.
  std::string where() const;

private:
  struct Spec;
  struct Source {
    std::filesystem::path file;
    std::size_t line = 0;
  };

  void command(Call &call) override;
  const Spec *match(std::span<const std::string_view> words) const;
  std::filesystem::path resolved(const std::filesystem::path &file) const;

  const Log &_log;
  std::vector<Spec> _specs;
  std::uint32_t _added = 0; // so a removed command's id is never handed out again
  CommandLog _session;
  std::vector<Source> _sourcing; // the files running, innermost last
  const Command _quit;
  const Command _source;
  const Command _log_save;
  bool _quitting = false;
};

} // namespace VP
