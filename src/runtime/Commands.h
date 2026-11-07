#pragma once

#include "runtime/Operator.h"

#include <filesystem>
#include <optional>
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
  // The same view as the next frame runs it, each recipe a node uses unfolded.
  virtual const View *unfolded(std::string_view name) = 0;
  // What each node of the view costs, as its running schedule measured it; none before
  // the view runs.
  virtual std::span<const Cost> costs(std::string_view name) = 0;
  // Each node's ports, as the running schedule bound them; none before the view runs.
  virtual std::span<const NodePort> ports(std::string_view name) = 0;
  // A project's manifest written as the changes so far left it, after a line a person
  // typed changed it, so its file shows each change; an app's and the library's views
  // stay as their files have them.
  virtual void write(std::string_view name) = 0;
  // Every view hosted, by the name a line addresses it with, the host's first.
  virtual std::vector<std::string> names() = 0;
  // A project's files that differ from what it kept as it was hosted or last saved, by
  // their paths from its folder, a folder's ending in /; none for an app's or the
  // library's view, which keeps none.
  virtual std::vector<std::string> unkept(std::string_view name) = 0;
  // A project's files put back as it kept them: each changed since written again, each
  // made since deleted, with the folders made for them.
  virtual void put_back(std::string_view name) = 0;
  // Takes a changed copy of the view. The schedule is rebuilt from it before the next
  // frame, once for every change since the last frame, on the path a live swap takes, so
  // it keeps what the changes left alone and checks only the view they leave. The
  // running schedule points into the old view until then, so a change never edits a view
  // in place.
  virtual void replace(std::string_view name, View view) = 0;
  // The name of the view whose manifest names the hosted view, empty for the one vulpen
  // started with; none when no view of that name is hosted.
  virtual std::optional<std::string> host_of(std::string_view name) = 0;
  // Around a line a person runs, which undo takes back as one step: begun before the
  // line runs, and ended after it, with the group of the session log its primitives
  // joined, none for none.
  virtual void begin_step(std::string_view line) = 0;
  virtual void end_step(std::optional<std::size_t> group) = 0;
  // The file port the lines reach files through, which keeps each change of a step.
  virtual FilePort &files() = 0;
  // How many steps undo can take back now.
  virtual std::size_t done() const = 0;

protected:
  ~ViewLookup() = default;
};

// Whether the log keeps a command's runs. A primitive edits a view or saves it, so
// replaying the primitives rebuilds the graph and the files saved from it (V08). Any
// other command only reads, or runs others, whose primitives the log keeps in its place.
// A save's file holds every edit of its view before it, which a restart replays no more.
enum class Primitive : std::uint8_t { no, edit, save };

// The session as groups: each line from outside the port keeps the primitives it ran,
// so a replay needs none of the commands that sent them (V08) and undo removes one
// group.
class CommandLog {
public:
  // Where a view's own file last held all of it: how many primitives the log kept then,
  // as the view was hosted or saved.
  struct Mark {
    std::string_view view; // by the name a line addresses it with, empty for the first
    std::size_t kept = 0;
  };

  // A line from outside the port, typed or read from a file: the primitives it runs
  // join one group.
  void open();
  // A line a part sends in answer to a group's line joins that group in place of one of
  // its own, until the next open, so what hides the line hides its answers too.
  void join(std::size_t group);
  // files: which of the words are files, as absolute paths; view: the name of the view
  // the primitive changed.
  void keep(std::vector<std::string> words,
            std::vector<std::size_t> files,
            std::string_view view,
            Primitive primitive);
  // Every primitive kept, a line each, in order: what source replays. A file is named
  // from the folder the log is saved in, where source resolves it, so a log moves with
  // the files it names (V08).
  std::string text(const std::filesystem::path &folder) const;
  // How many primitives it kept so far, those of groups undo took back too, so a mark
  // stays where it was.
  std::size_t size() const;
  // The group the primitives of the line running join.
  std::size_t group() const;
  // A group undo took back, or redo brought back: what the log hands on passes over a
  // group taken back, as if its line never ran.
  void take_back(std::size_t group, bool taken);
  // The views the primitives kept since the first so many changed, each once.
  std::vector<std::string> views_since(std::size_t from) const;
  // What the views' files do not hold yet: the edits of each view marked, kept since its
  // mark, in order, as text names them; a view not marked holds none, as one no longer
  // hosted. What a restart replays (V08).
  std::string after(std::span<const Mark> marks, const std::filesystem::path &folder) const;

private:
  struct Kept {
    std::vector<std::string> words;
    std::vector<std::size_t> files;
    std::string view;
    Primitive primitive = Primitive::edit;
  };

  static std::string line(const Kept &kept, const std::filesystem::path &folder);

  std::vector<std::vector<Kept>> _groups;
  std::vector<bool> _taken; // by _groups: undo took it back
  std::optional<std::size_t> _joined; // the group a part's answer joins, while it runs
  std::size_t _size = 0;
};

// Every change to a view is a command through this one port, so the CLI, a GUI, a
// script and an agent act alike, and replaying the log rebuilds the session. A command
// registers with its usage, whose placeholders give its completion, and its help, or
// not at all (RV04). A usage's last placeholder may end in ..., taking one argument or
// more, and stand in brackets, which a line may leave out: node add <name>
// [<word=value>...]. A <file> argument reaches its command resolved, as an absolute path,
// a relative one counting from the folder a person typed it at, beside the script
// running it, or from where vulpen started; so does a <project> or a <path> that holds a
// /, and one without names a project or a file the view holds, and comes as typed.
//
// A line addresses a hosted view by its name, as `<name>: <command>`, and `:` alone
// addresses the view vulpen started with, which a line naming none addresses too. A line
// a command sends addresses the same view unless it names one, and the log keeps each
// primitive with the name of its view, so every line of it stands alone (V08).
class Commands final : public CommandPort, public CommandHandler {
public:
  // files and processes: what a command reaches files and programs through.
  Commands(const Log &log, FilePort &files, ProcessPort &processes);
  ~Commands();
  Commands(const Commands &) = delete;
  Commands &operator=(const Commands &) = delete;

  // usage: the command's words as a person types them. Throws naming the mistake, so a
  // command without its usage and help never registers (A02). view: the view whose node
  // registers it, by its name, which may hold a command another view holds too, as two
  // projects' nodes of one name; none for the engine's own, which every view shares.
  Command add(std::string_view usage,
              std::string_view help,
              CommandHandler &handler,
              Primitive primitive = Primitive::no,
              std::optional<std::string_view> view = std::nullopt);
  // Before the command's handler goes, so no run reaches it after.
  void remove(Command command);
  // A line from outside the port, in a group of its own; returns what its command
  // answers. typed_at: the folder a person typed it at, which its relative paths count
  // from, as send_typed's. Throws naming the mistake: a command nobody registered, or
  // words its usage does not take.
  std::string run(std::string_view line, std::string_view typed_at = {});
  std::string send(std::string_view line) override;
  std::string send_typed(std::string_view line, std::string_view folder) override;
  std::vector<Usage> usages() const override;
  std::span<const Logged> log() const override;
  void keep_log_to(std::span<const std::string> views) override;
  std::string_view current() override;
  // Before a frame cooks: what the log printed since the frame before is what nodes
  // read in this one, and the frame's holds start anew.
  void frame();
  // A node's Cook::hold: work the end of the run would lose, which quit refuses for while
  // a node held it this frame or the one before, naming the node and what.
  void hold(std::string_view node, std::string_view what);
  // Runs a file's lines in turn until one quits, each in a group of its own, so undo
  // after a replay steps back one command at a time, and prints what each answers.
  // Throws naming the file and line of the first that fails, and runs none after it, so
  // a script never goes on from a state it did not expect.
  void source(const std::filesystem::path &file, bool past_refusals = false);
  // Lines the log kept already, run again, as the edits a manifest read again does not
  // hold are replayed over it: the log keeps none of them twice, so a replay of the log
  // runs each once (V08). A line that fails is passed over; returns each, with why.
  std::vector<std::string> replay(std::string_view lines);
  // Whether a replay runs, so the views host and let go of views once it ran.
  bool replaying() const;
  // Where a command finds the view it addresses, once the views exist.
  void look_in(ViewLookup &views);
  // The file port its commands reach files through, which the views keep a project's by.
  FilePort &files();
  // A hosted view by its name, as the changes so far left it, which a node reads while it
  // cooks; null when no view has the name, or none exists yet.
  const View *view(std::string_view name);
  // The same view as the next frame runs it, each recipe a node uses unfolded.
  const View *unfolded(std::string_view name);
  // What each node of the view costs, which a node reads while it cooks.
  std::span<const Cost> costs(std::string_view name);
  std::span<const NodePort> ports(std::string_view name);
  // Whether quit, or quit force, ran, so the run ends before its next frame.
  bool quitting() const;
  // This frame's holds, then those of the frame before, `node: what` a line, each once:
  // the work the end of the run would lose.
  std::vector<std::string_view> holding() const;
  // The session's log, which says how far a view's file holds it, and what a restart
  // replays.
  const CommandLog &session() const;
  // The view the running primitive changes, when not the one its line addresses, as view
  // close changes the host of the view it closes: the log keeps the primitive under it,
  // so a restart replays it while that view is hosted.
  void changes(std::string_view view);
  // The file and line running, as `script.txt:7`; empty for a line typed or sent.
  std::string where() const;
  // The name of the view the running command addresses; empty for the one vulpen
  // started with.
  std::string_view addressed() const;
  std::size_t done() const override;
  // A group undo took back, or redo brought back (CommandLog::take_back).
  void take_back(std::size_t group, bool taken);

private:
  struct Spec;
  struct Source {
    std::filesystem::path file;
    std::size_t line = 0;
  };
  // A frame's holds, `node: what` a line. The first count are the frame's, and the rest
  // are written over next, so a hold each frame allocates nothing once they grew (CPP10).
  struct Holds {
    std::vector<std::string> lines;
    std::size_t count = 0;
  };

  void command(Call &call) override;
  std::string answer(std::string_view line, std::string_view folder, bool typed);
  void quit(bool force);
  void write_since(std::size_t from);
  const Spec *match(std::span<const std::string_view> words,
                    std::string_view address) const;
  std::string view_named(std::vector<std::string_view> &words) const;
  std::vector<std::string> resolve(const Spec &spec,
                                   std::vector<std::string_view> &arguments,
                                   const std::filesystem::path &from) const;
  void switch_to(std::string_view name);
  std::filesystem::path paths_from(std::string_view typed_at) const;
  std::filesystem::path resolved(const std::filesystem::path &file) const;

  const Log &_log;
  FilePort &_files;
  ProcessPort &_processes;
  std::vector<Spec> _specs;
  std::uint32_t _added = 0; // so a removed command's id is never handed out again
  ViewLookup *_views = nullptr;
  CommandLog _session;
  std::vector<Source> _sourcing; // the files running, innermost last
  std::string _addressed;
  std::string _changed; // by the primitive running
  std::size_t _depth = 0; // the lines running, each sent by the one before it
  // The group of the last line a person ran that kept edits, and the frame it ran: the
  // parts' lines of that frame and the few after answer it, and join its group. After an
  // undo or a redo, none: its answers are kept nowhere, as the views it put back hold
  // what they do, which the parts work out again from them.
  std::optional<std::size_t> _answered;
  std::uint64_t _answered_frame = 0;
  std::uint64_t _frames = 0;
  bool _answers_undo = false; // the frames since are an undo's or a redo's
  bool _joining = false;      // the line about to run joins _answered's group
  bool _discarding = false;   // the line running keeps nothing
  bool _replaying = false; // what runs, the log kept already
  const Command _quit;
  const Command _quit_force;
  const Command _source;
  const Command _log_save;
  const Command _switch;
  bool _quitting = false;
  std::string _current;               // the view view switch moved to
  std::vector<std::string> _holding; // the views that held it then, its host first
  Holds _holds; // this frame's
  Holds _held;  // the frame before's
  std::vector<Logged> _logged; // the first _logged_count are this frame's
  std::size_t _logged_count = 0;
};

} // namespace VP
