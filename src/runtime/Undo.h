#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace VP {

class Call;
class Commands;
class FilePort;
class ViewLookup;
struct View;

// A file larger than this neither a step nor a project keeps, so its data files are never
// held in memory twice; undo refuses a step that changed one, and view close force and
// quit force leave one as it is.
inline constexpr std::uintmax_t kept_most = 16 * 1024 * 1024;

// The lines a person ran, as steps undo takes back and redo runs again: each view a line
// changed as it was before the line and after, and each change it made to files through
// the port the lines reach files through. The hosted views are the views', which a step
// reaches through the port's lookup (A01), so a step brings back what it changed and
// nothing else.
class Undo final {
public:
  // files: the command port's own; views: the hosted views, which a step comes back to;
  // port: the session log, whose group a step's primitives joined.
  Undo(FilePort &files, ViewLookup &views, Commands &port);
  ~Undo();
  Undo(const Undo &) = delete;
  Undo &operator=(const Undo &) = delete;

  // Around a line a person runs, as the port's lookup says (ViewLookup::begin_step).
  void begin(std::string_view line);
  void end(std::optional<std::size_t> group);
  // A view the line runs changes, as it was before its first change, and whether it is a
  // project, as a step keeps an app's view only while the line changed what is hosted.
  void note(std::string_view name, bool project, const View &before);
  // The file port the lines reach files through, which keeps each change of a step.
  FilePort &files();
  // How many steps undo can take back now.
  std::size_t done() const;
  // undo, or redo when again, answering what it took back or ran again.
  void take_back(Call &call, bool again);

private:
  // A line a person ran, and the port that keeps its changes to files.
  struct Step;
  class Recorded;

  void check(const Step &step, bool again);
  void take_files_back(const Step &step);
  void bring_files_again(const Step &step);
  void remove_tree(const std::filesystem::path &path);
  void restore_views(const Step &step, bool again);
  bool kept_by_view(const Step &step, const std::filesystem::path &file, bool again);

  ViewLookup &_views;
  Commands &_port;
  std::unique_ptr<Recorded> _recorded; // over the command port's files
  std::unique_ptr<Step> _step;         // the line a person runs now; null for none
  std::vector<Step> _done;             // the steps undo takes back, the newest last
  std::vector<Step> _undone;           // those it took back, which redo runs again
};

} // namespace VP
