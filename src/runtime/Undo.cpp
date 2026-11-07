#include "runtime/Undo.h"

#include "runtime/Commands.h"
#include "runtime/View.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace VP {

// The lines undo takes back at most, so memory stays bounded (A03); a step keeps no file
// past kept_most either, and undo refuses its line, naming the file.
constexpr std::size_t undo_room = 100;

// A line a person ran: each view it changed as it was before the line and after, and each
// change it made to files through the port, in order, with what undo and redo need to
// make it either way.
struct Undo::Step {
  struct Viewed {
    std::string name;
    bool project = false;
    View before;
    std::optional<View> after; // none once the line closed it
  };
  // A save, with the text it found, none for no file, and the text it wrote; a remove of
  // a file with its text, or of an empty folder with none; a copy or a rename to another
  // path; a folder made. Paths are absolute.
  struct Change {
    enum class Kind : std::uint8_t { save, remove, copy, rename, make };
    Kind kind = Kind::save;
    std::filesystem::path path;
    std::filesystem::path to;
    std::optional<std::string> before;
    std::string after;
  };
  // A folder above what it changed, as it was before the line: whether it was there, as
  // the file port takes a folder its last file leaves.
  struct Folder {
    std::filesystem::path path;
    bool there = false;
  };
  std::string line;
  std::optional<std::size_t> group; // of the session log, which its primitives joined
  std::vector<std::string> hosted;  // the views hosted as the line began
  std::vector<std::string> left;    // and as it ended
  std::vector<Viewed> views;
  std::vector<Change> changes;
  std::vector<Folder> folders;     // each once
  std::filesystem::path too_large; // a file past kept_most, which undo cannot bring back
};

// Through to the file port, keeping each change the line of a step makes, but one of a
// hidden file, as the parts keep in .vulpen/.
class Undo::Recorded final : public FilePort {
public:
  Recorded(FilePort &files, Undo &undo) : _files(files), _undo(undo) {}

private:
  std::string_view text(File file) override {
    return _files.text(file);
  }
  void save(File file, std::string_view text) override {
    _files.save(file, text);
  }
  std::string read(std::string_view file) override {
    return _files.read(file);
  }
  void save(std::string_view file, std::string_view text) override {
    if (Step::Change *const change = noted(Step::Change::Kind::save, file)) {
      change->before = held(file);
      change->after = text;
    }
    _files.save(file, text);
  }
  void remove(std::string_view file) override {
    if (Step::Change *const change = noted(Step::Change::Kind::remove, file))
      change->before = held(file);
    _files.remove(file);
  }
  std::vector<std::string> list(std::string_view folder) override {
    return _files.list(folder);
  }
  void copy(std::string_view from, std::string_view to) override {
    noted(Step::Change::Kind::copy, from, to);
    _files.copy(from, to);
  }
  void rename(std::string_view from, std::string_view to) override {
    noted(Step::Change::Kind::rename, from, to);
    _files.rename(from, to);
  }
  void save(const View &view, std::string_view file) override {
    Step::Change *const change = noted(Step::Change::Kind::save, file);
    if (change)
      change->before = held(file);
    _files.save(view, file);
    if (change)
      change->after = _files.read(file);
  }
  View manifest(std::string_view file) override {
    return _files.manifest(file);
  }
  void make(std::string_view folder) override {
    noted(Step::Change::Kind::make, folder);
    _files.make(folder);
  }
  std::span<const Moved> moved() const override {
    return _files.moved();
  }

  // The change, kept with the folders above its paths as they are now; null while no
  // step records, and for a hidden file.
  Step::Change *
  noted(Step::Change::Kind kind, std::string_view path, std::string_view to = {}) {
    Step *const step = _undo._step.get();
    if (!step || hidden(path) || (!to.empty() && hidden(to)))
      return nullptr;
    for (const std::string_view at : {path, to})
      for (std::filesystem::path folder = std::filesystem::path(at).parent_path();
           !at.empty() && !folder.empty();
           folder = folder.parent_path()) {
        if (std::ranges::find(step->folders, folder, &Step::Folder::path) !=
            step->folders.end())
          break;
        std::error_code unread;
        const bool there = std::filesystem::is_directory(folder, unread);
        step->folders.push_back({folder, there});
        if (there || folder == folder.parent_path())
          break;
      }
    return &step->changes.emplace_back(Step::Change{.kind = kind, .path = path, .to = to});
  }

  // A file's text, none for no file or a folder; one past kept_most is noted, so undo
  // refuses the step.
  std::optional<std::string> held(std::string_view file) {
    std::error_code unread;
    if (!std::filesystem::is_regular_file(file, unread))
      return std::nullopt;
    if (std::filesystem::file_size(file, unread) > kept_most || unread) {
      _undo._step->too_large = file;
      return std::nullopt;
    }
    return _files.read(file);
  }

  FilePort &_files;
  Undo &_undo;
};

Undo::Undo(FilePort &files, ViewLookup &views, Commands &port)
    : _views(views), _port(port), _recorded(std::make_unique<Recorded>(files, *this)) {}

Undo::~Undo() = default;

void Undo::begin(std::string_view line) {
  _step = std::make_unique<Step>(Step{.line = std::string(line), .hosted = _views.names()});
}

void Undo::note(std::string_view name, bool project, const View &before) {
  if (_step && std::ranges::none_of(_step->views, [&](const Step::Viewed &viewed) {
        return viewed.name == name;
      }))
    _step->views.push_back({.name = std::string(name), .project = project, .before = before});
}

// What the line changed, as it left it. An app's view counts only while the line changed
// which views are hosted, so a line that only moved what a GUI shows, as cd and select
// do, is no step, nor is one that changed nothing; a new step leaves nothing to redo.
void Undo::end(std::optional<std::size_t> group) {
  std::unique_ptr<Step> step = std::move(_step);
  if (!step)
    return;
  const bool hosting = _views.names() != step->hosted;
  std::erase_if(step->views,
                [&](const Step::Viewed &viewed) { return !viewed.project && !hosting; });
  for (Step::Viewed &viewed : step->views)
    if (const View *const found = _views.find(viewed.name))
      viewed.after = *found;
  step->group = group;
  step->left = _views.names();
  if (step->views.empty() && step->changes.empty())
    return;
  _done.push_back(std::move(*step));
  if (_done.size() > undo_room)
    _done.erase(_done.begin());
  _undone.clear();
}

FilePort &Undo::files() {
  return *_recorded;
}

std::size_t Undo::done() const {
  return _done.size();
}

namespace {

bool there(const std::filesystem::path &path) {
  std::error_code unread;
  return std::filesystem::exists(path, unread);
}

} // namespace

// The files first, so a view the line closed comes back with its manifest there, then the
// views; each project written, as a typed line's are. What would lose a change made since
// is refused before anything changes.
void Undo::take_back(Call &call, bool again) {
  _step.reset(); // undo and redo are no steps of their own
  std::vector<Step> &from = again ? _undone : _done;
  if (from.empty())
    throw std::runtime_error(
        again ? "nothing to redo: redo runs again what undo took back last, until "
                "another line changes a project"
              : "nothing to undo: undo takes back a line typed, picked from a menu or "
                "sent by the graph's keys, that changed a project or its files");
  if (!from.back().too_large.empty())
    throw std::runtime_error(std::format(
        "{} is past the {} MB a step keeps of a file, so {} cannot bring it back; "
        "nothing changed",
        from.back().too_large.string(),
        kept_most >> 20,
        again ? "redo" : "undo"));
  check(from.back(), again);
  Step step = std::move(from.back());
  from.pop_back();
  if (again)
    bring_files_again(step);
  else
    take_files_back(step);
  restore_views(step, again);
  for (const Step::Viewed &viewed : step.views)
    if (viewed.project)
      _views.write(viewed.name);
  if (step.group)
    _port.take_back(*step.group, !again);
  call.reply(std::format("{} {}", again ? "redid" : "undid", step.line));
  (again ? _done : _undone).push_back(std::move(step));
}

// Each path as the step left it, for undo, or found it, for redo, by its last change or
// its first, so neither writes over a change made since, by a line or another program.
void Undo::check(const Step &step, bool again) {
  std::vector<std::filesystem::path> seen;
  const auto holds = [&](const std::filesystem::path &file, std::string_view text) {
    std::error_code unread;
    return std::filesystem::is_regular_file(file, unread) &&
           static_cast<FilePort &>(*_recorded).read(file.string()) == text;
  };
  const auto look = [&](const Step::Change &change) {
    using Kind = Step::Change::Kind;
    if ((change.kind != Kind::save && change.kind != Kind::remove) ||
        std::ranges::find(seen, change.path) != seen.end() ||
        kept_by_view(step, change.path, again))
      return;
    seen.push_back(change.path);
    const bool saved = change.kind == Kind::save;
    const bool as_left = saved ? holds(change.path, change.after) : !there(change.path);
    const bool as_found =
        change.before ? holds(change.path, *change.before) : there(change.path) != saved;
    if (again ? !as_found : !as_left)
      throw std::runtime_error(std::format("{} changed since {}, which {} would lose; "
                                           "nothing changed",
                                           change.path.string(),
                                           step.line,
                                           again ? "redo" : "undo"));
  };
  if (again)
    std::ranges::for_each(step.changes, look);
  else
    std::ranges::for_each(step.changes | std::views::reverse, look);
}

// The last change first. A hosted view's manifest is the view's, written as it comes
// back. Then each folder the line made goes once empty, deepest first, and each that was
// there comes back if a delete took it, as the port takes a folder its last file leaves.
void Undo::take_files_back(const Step &step) {
  using Kind = Step::Change::Kind;
  FilePort &files = *_recorded;
  for (const Step::Change &change : step.changes | std::views::reverse) {
    const std::string path = change.path.string();
    if (change.kind == Kind::save && !kept_by_view(step, change.path, false))
      change.before ? files.save(path, *change.before) : files.remove(path);
    else if (change.kind == Kind::remove)
      change.before ? files.save(path, *change.before) : files.make(path);
    else if (change.kind == Kind::copy)
      remove_tree(change.to);
    else if (change.kind == Kind::rename)
      files.rename(change.to.string(), path);
    else if (change.kind == Kind::make)
      remove_tree(change.path);
  }
  std::vector<const Step::Folder *> folders;
  for (const Step::Folder &folder : step.folders)
    folders.push_back(&folder);
  std::ranges::sort(folders, std::greater{}, [](const Step::Folder *folder) {
    return std::ranges::distance(folder->path);
  });
  for (const Step::Folder *const folder : folders) {
    std::error_code unread;
    if (!folder->there && std::filesystem::is_directory(folder->path, unread) &&
        std::filesystem::is_empty(folder->path, unread))
      files.remove(folder->path.string());
  }
  for (const Step::Folder *const folder : folders | std::views::reverse)
    if (folder->there && !there(folder->path))
      files.make(folder->path.string());
}

void Undo::bring_files_again(const Step &step) {
  using Kind = Step::Change::Kind;
  FilePort &files = *_recorded;
  for (const Step::Change &change : step.changes) {
    const std::string path = change.path.string();
    if (change.kind == Kind::save && !kept_by_view(step, change.path, true))
      files.save(path, change.after);
    else if (change.kind == Kind::remove && there(change.path))
      files.remove(path);
    else if (change.kind == Kind::copy)
      files.copy(path, change.to.string());
    else if (change.kind == Kind::rename)
      files.rename(path, change.to.string());
    else if (change.kind == Kind::make)
      files.make(path);
  }
}

// What a copy or a made folder left, deepest first, through the port, which takes a
// folder its last file leaves.
void Undo::remove_tree(const std::filesystem::path &path) {
  FilePort &files = *_recorded;
  for (std::string name : files.list(path.string())) {
    if (name.ends_with('/'))
      name.pop_back();
    remove_tree(path / name);
  }
  if (there(path))
    files.remove(path.string());
}

// The views that host others first, so a view the line closed is hosted again before its
// own state comes back; an app's view only in what it hosts.
void Undo::restore_views(const Step &step, bool again) {
  for (const bool whole : {false, true})
    for (const Step::Viewed &viewed : step.views) {
      const View *const found = _views.find(viewed.name);
      if (viewed.project != whole || !found || (again && !viewed.after))
        continue;
      View back = whole ? (again ? *viewed.after : viewed.before) : *found;
      if (!whole)
        back.children = (again ? *viewed.after : viewed.before).children;
      _views.replace(viewed.name, std::move(back));
    }
}

// A manifest a view hosted now holds, which stays hosted on the side undo or redo goes
// to: the view comes back whole and is written, so its file is left to it; the manifest
// of a view the step hosted or let go is a file as any.
bool Undo::kept_by_view(const Step &step, const std::filesystem::path &file, bool again) {
  const std::vector<std::string> &hosting = again ? step.left : step.hosted;
  return std::ranges::any_of(hosting, [&](const std::string &name) {
    const View *const hosted = _views.find(name);
    return hosted && hosted->file.lexically_normal() == file.lexically_normal();
  });
}

} // namespace VP
