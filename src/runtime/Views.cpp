#include "runtime/Views.h"

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace VP {

namespace {

// When the file was last written. One that is gone reads as never written, so the next
// build tries it again.
std::filesystem::file_time_type stamp(const std::filesystem::path &file) {
  std::error_code gone;
  return std::filesystem::last_write_time(file, gone);
}

// A project: a view in a folder of its own, not an app beside the library nor a recipe in
// it, which build from the recipes' folder.
bool project(const View &view) {
  return !view.file.empty() && Manifest::root(view) == view.file.parent_path();
}

// A file larger than this a project does not keep, so its data files are never held in
// memory twice; view close force and quit force leave such a file as it is.
constexpr std::uintmax_t kept_most = 16 * 1024 * 1024;

// A project's files and folders, each by its path from the project's folder, a folder's
// ending in /, but the hidden ones, as .vulpen/, where the parts keep what a run
// rewrites as it goes; and apart, the files too large or unreadable to keep.
struct Kept {
  bool there = false; // the project's folder
  std::map<std::string, std::string, std::less<>> files;
  std::set<std::string, std::less<>> folders;
  std::vector<std::string> left;
};

void walk(FilePort &files,
          const std::filesystem::path &folder,
          const std::string &inside,
          Kept &kept) {
  for (const std::string &name : files.list((folder / inside).string())) {
    if (name.starts_with('.'))
      continue;
    std::string path = inside + name;
    std::error_code unread;
    if (name.ends_with('/')) {
      walk(files, folder, path, kept);
      kept.folders.insert(std::move(path));
    } else if (std::filesystem::file_size(folder / path, unread) > kept_most || unread) {
      kept.left.push_back(std::move(path));
    } else {
      try {
        kept.files.emplace(path, files.read((folder / path).string()));
      } catch (const std::runtime_error &) {
        kept.left.push_back(std::move(path)); // gone or locked mid-walk
      }
    }
  }
}

// A project's files as they are now; none for a view that is no project.
Kept kept_of(FilePort &files, const View &view) {
  Kept kept;
  std::error_code missing;
  if (!project(view) || !std::filesystem::is_directory(view.file.parent_path(), missing))
    return kept;
  kept.there = true;
  walk(files, view.file.parent_path(), {}, kept);
  return kept;
}

// What differs from what was kept, sorted: a file changed, made or deleted since, and a
// folder made or deleted since.
std::vector<std::string> differences(const Kept &kept, const Kept &now) {
  std::vector<std::string> changed;
  for (const auto &[path, text] : now.files)
    if (const auto found = kept.files.find(path);
        found == kept.files.end() || found->second != text)
      changed.push_back(path);
  for (const auto &[path, text] : kept.files)
    if (!now.files.contains(path))
      changed.push_back(path);
  for (const std::string &folder : now.folders)
    if (!kept.folders.contains(folder))
      changed.push_back(folder);
  for (const std::string &folder : kept.folders)
    if (!now.folders.contains(folder))
      changed.push_back(folder);
  std::ranges::sort(changed);
  return changed;
}

// The files made since deleted, the folders made for them after them, deepest first, as
// a/b/ sorts after a/, and the files changed or deleted since written as kept; a folder
// kept that a delete emptied and took made again. The port deletes and writes, so an
// editor showing one of them follows (RA04: each write a temp renamed over the file).
void put_back(FilePort &files, const std::filesystem::path &folder, const Kept &kept) {
  Kept now;
  walk(files, folder, {}, now);
  for (const auto &[path, text] : now.files)
    if (!kept.files.contains(path))
      files.remove((folder / path).string());
  for (auto made = now.folders.rbegin(); made != now.folders.rend(); ++made) {
    std::error_code gone;
    if (!kept.folders.contains(*made) &&
        std::filesystem::is_empty(folder / *made, gone) && !gone)
      files.remove((folder / *made).string());
  }
  for (const auto &[path, text] : kept.files)
    if (const auto found = now.files.find(path);
        found == now.files.end() || found->second != text)
      files.save((folder / path).string(), text);
  for (const std::string &held : kept.folders) {
    std::error_code missing;
    if (!std::filesystem::exists(folder / held, missing))
      files.make((folder / held).string());
  }
  std::error_code missing;
  if (kept.there && !std::filesystem::exists(folder, missing))
    files.make(folder.string());
}

// Throws naming what keeps it from running: a recipe the library lacks, or a cycle that
// runs through the nodes of a recipe a node uses, which no single edit could show.
std::unique_ptr<View> flat_of(const View &view, Recipes &recipes) {
  auto flat = std::make_unique<View>(Manifest::flatten(view, recipes));
  Schedule::order(*flat);
  return flat;
}

// Each line names its manifest in its folder, as a node's errors do.
void note(const Log &log,
          const View &view,
          Level level,
          const std::vector<std::string> &notes) {
  const std::string file =
      (view.file.parent_path().filename() / view.file.filename()).generic_string();
  for (const std::string &text : notes)
    log.write(level, Tag::run, std::format("{}: {}", file, text));
}

} // namespace

// One view and its schedule. Edits and save work on the view as its manifest says, and
// the schedule runs it with the recipes its nodes use unfolded, so it outlives the
// schedule.
struct Views::Hosted {
  std::string name;   // empty for the view vulpen started with
  std::string parent; // the name of the view whose manifest names it
  std::unique_ptr<View> view;
  std::unique_ptr<View> flat;
  // What commands made of the view since, both ways; null when nothing.
  std::unique_ptr<View> edited;
  std::unique_ptr<View> edited_flat;
  std::filesystem::file_time_type read; // the manifest's, when last read or saved
  // How many primitives the log kept when the manifest last held all of the view: as it
  // was hosted or saved. Its edits since then are what a restart replays, and a read
  // again.
  std::size_t marked = 0;
  std::unique_ptr<Schedule> schedule; // null until the frame after it was hosted
  // Where its schedule's draws into its window go.
  Schedule::Into into = Schedule::Into::window;
  // Its files as at its hosting or last save, which a forced close or quit puts back;
  // none for an app or a library view.
  Kept kept;
  // It has taken the window of a view beside it, so it keeps the window until it closes.
  bool presented = false;
  bool removed = false; // its schedule goes before the next frame

  const View &current() const {
    return edited ? *edited : *view;
  }
  // The view the next frame runs.
  const View &next() const {
    return edited ? *edited_flat : *flat;
  }
};

// The view vulpen started with is built before any command registers, so no command
// is left to a handler whose constructor threw.
Views::Views(const Wiring &wiring, View view, Commands &commands, Recipes &recipes)
    : _wiring(wiring), _port(commands), _recipes(recipes), _hosted([&] {
        std::vector<std::unique_ptr<Hosted>> hosted;
        Hosted &host = *hosted.emplace_back(std::make_unique<Hosted>());
        host.read = stamp(view.file);
        host.kept = kept_of(commands.files(), view);
        host.view = std::make_unique<View>(std::move(view));
        host.flat = flat_of(*host.view, recipes);
        host.schedule =
            std::make_unique<Schedule>(wiring, "", *host.flat, Schedule::Into::window);
        return hosted;
      }()),
      _schedules{_hosted.front()->schedule.get()} {
  follow({}, _hosted.front()->current());
  _save = commands.add("view save",
                       "writes the view over its manifest, keeping the comments in it, "
                       "and keeps a project's files as they are, which view close force "
                       "and quit force put back; a line typed writes the manifest anyway",
                       *this,
                       Primitive::save);
  _list = commands.add("view list",
                       "lists the views this view hosts, with their files, in the order "
                       "hosted",
                       *this);
  _clear = commands.add("image clear <port>",
                        "drops the pixels of an image a node's C++ fills, named by that "
                        "port or one that samples it, until the node fills it again",
                        *this);
}

Views::~Views() = default;

View Views::read(const Log &log, const std::filesystem::path &file) {
  View view = Manifest::load(file);
  note(log, view, Level::info, Manifest::refresh(view));
  note(log, view, Level::warn, Manifest::unnamed(view));
  return view;
}

const View &Views::view() const {
  return _hosted.front()->current();
}

bool Views::shows() const {
  const Hosted *const owner = this->owner();
  return std::ranges::any_of(_hosted, [&](const std::unique_ptr<Hosted> &hosted) {
    return !hosted->removed && target(*hosted, owner) == nullptr &&
           draws_into_window(hosted->next());
  });
}

// One rebuild for all the changes since the last frame, so a script is checked as a
// whole, and a replay costs about what a load does. The new schedule takes from the old
// one, which still reads the old view, so the old view goes last. A view comes after the
// view hosting it, so where its draws go follows its host's view as rebuilt. The window's
// owner is chosen once, before any view is built, as shows() chose it, so a view built
// now for the first time is none of the views it chooses from.
bool Views::rebuild() {
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    hosted->presented = hosted->presented || (!hosted->removed && presents(*hosted));
  _owner = owner();
  bool changed = false;
  for (auto at = _hosted.begin(); at != _hosted.end();) {
    Hosted &hosted = **at;
    if (hosted.removed) {
      if (hosted.schedule)
        _wiring.log.write(Level::info,
                          Tag::run,
                          std::format("child {}: no longer hosted", hosted.name));
      at = _hosted.erase(at);
      changed = true;
      continue;
    }
    const std::optional<const Hosted *> to = target(hosted, _owner);
    const Schedule::Into into = !to   ? Schedule::Into::none
                                : *to ? Schedule::Into::image
                                      : Schedule::Into::window;
    // So a project that shows nothing says why.
    if ((into == Schedule::Into::none) != (hosted.into == Schedule::Into::none) &&
        draws_into_window(hosted.next()))
      _wiring.log.write(
          Level::info,
          Tag::run,
          into == Schedule::Into::none
              ? std::format(
                    "child {}: draws nowhere, since {}", hosted.name, nowhere(hosted))
              : std::format("child {}: draws again", hosted.name));
    if (hosted.edited) {
      const bool added = !hosted.schedule;
      hosted.schedule = std::make_unique<Schedule>(
          _wiring, hosted.name, *hosted.edited_flat, into, hosted.schedule.get());
      hosted.view = std::move(hosted.edited);
      hosted.flat = std::move(hosted.edited_flat);
      if (added)
        _wiring.log.write(Level::info,
                          Tag::run,
                          std::format("child {}: hosted from {}",
                                      hosted.name,
                                      hosted.view->file.string()));
      changed = true;
    } else if (into != hosted.into) {
      // The same view, its draws' pipelines made again for what they draw into now.
      hosted.schedule = std::make_unique<Schedule>(
          _wiring, hosted.name, *hosted.flat, into, hosted.schedule.get());
      changed = true;
    }
    hosted.into = into;
    ++at;
  }
  if (changed) {
    _schedules.clear();
    for (const std::unique_ptr<Hosted> &hosted : _hosted)
      _schedules.push_back(hosted->schedule.get());
  }
  return changed;
}

std::span<Schedule *const> Views::schedules() const {
  return _schedules;
}

void Views::gather(std::vector<Pass> &passes) const {
  std::vector<const Hosted *> gathered;
  gather(*_hosted.front(), passes, gathered);
  draw_into(nullptr, nullptr, passes);
}

// A view comes after the view hosting it, so going back from the last, each view it hosts
// comes before the views hosted before it, as each view's work comes before its own.
void Views::gather(const Hosted &hosted,
                   std::vector<Pass> &passes,
                   std::vector<const Hosted *> &gathered) const {
  gathered.push_back(&hosted);
  for (const std::unique_ptr<Hosted> &other : _hosted | std::views::reverse)
    if (!other->removed && std::ranges::find(gathered, other.get()) == gathered.end() &&
        ((!other->name.empty() && other->parent == hosted.name) ||
         taker(*other) == &hosted))
      gather(*other, passes, gathered);
  // A view that draws nowhere still runs its work: its C++ cooks and its dispatches run.
  if (!hosted.schedule)
    return;
  hosted.schedule->work(passes);
  // The view taking its window made the image as it made the connection that takes it.
  if (const Hosted *const by = taker(hosted); by && by->schedule)
    if (const Offscreen *const image = by->schedule->window_of(hosted.name))
      draw_into(&hosted, image, passes);
}

void Views::draw_into(const Hosted *into,
                      const Offscreen *image,
                      std::vector<Pass> &passes) const {
  bool drawn = false;
  for (const std::unique_ptr<Hosted> &at : _hosted)
    if (!at->removed && at->schedule && target(*at, _owner) == into)
      drawn |= at->schedule->draws(passes, image);
  // A window no draw renders into, as a view's whose draws are all refused, still takes
  // its render pass, which clears it, so what samples it shows nothing rather than what
  // its memory held before, as another view's frame.
  if (image && !drawn)
    Schedule::clears(passes, image);
}

// The host first, so a view its host and one beside it both take goes to its host.
const Views::Hosted *Views::taker(const Hosted &hosted) const {
  if (hosted.name.empty())
    return nullptr;
  for (const std::unique_ptr<Hosted> &other : _hosted)
    if (!other->removed && other.get() != &hosted &&
        (other->name == hosted.parent ||
         (!other->name.empty() && other->parent == hosted.parent)) &&
        std::ranges::any_of(other->next().connections, [&](const Connection &connection) {
          return connection.from.view() && connection.from.node == hosted.name;
        }))
      return other.get();
  return nullptr;
}

bool Views::presents(const Hosted &hosted) const {
  const auto beside = [&](const Connection &connection) {
    const Hosted *const from =
        connection.from.view() ? this->hosted(connection.from.node) : nullptr;
    return from && from != &hosted && from->parent == hosted.parent;
  };
  return !hosted.name.empty() && std::ranges::any_of(hosted.next().connections, beside);
}

bool Views::draws(const Hosted &hosted) const {
  return draws_into_window(hosted.next()) ||
         std::ranges::any_of(_hosted, [&](const std::unique_ptr<Hosted> &other) {
           return !other->removed && !other->name.empty() &&
                  other->parent == hosted.name && !taker(*other) && draws(*other);
         });
}

// A view without a schedule is built this frame for the first time.
const Views::Hosted *Views::owner() const {
  const auto top = [](const Hosted &hosted) {
    return !hosted.removed && !hosted.name.empty() && hosted.parent.empty();
  };
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (top(*hosted) && hosted->schedule && (hosted->presented || presents(*hosted)))
      return hosted.get();
  const Hosted *holding = hosted(_port.current());
  while (holding && !holding->parent.empty())
    holding = hosted(holding->parent);
  if (holding && top(*holding) && holding->schedule && draws(*holding))
    return holding;
  for (const bool ran : {true, false})
    for (const std::unique_ptr<Hosted> &hosted : _hosted)
      if (top(*hosted) && (hosted->schedule || !ran) && draws(*hosted))
        return hosted.get();
  return nullptr;
}

std::optional<const Views::Hosted *> Views::target(const Hosted &hosted,
                                                   const Hosted *owner) const {
  if (hosted.name.empty())
    return nullptr;
  if (taker(hosted))
    return &hosted;
  const Hosted *const host = this->hosted(hosted.parent);
  if (!host)
    return nullptr;
  if (draws_into_window(host->next()) || (host->name.empty() && &hosted != owner))
    return std::nullopt;
  return target(*host, owner);
}

// Walking up as target does, to where it stopped.
std::string_view Views::nowhere(const Hosted &hosted) const {
  const Hosted &host = *this->hosted(hosted.parent);
  if (draws_into_window(host.next()))
    return "its host draws there and no connection takes its window";
  if (host.name.empty())
    return "the window shows another view";
  return nowhere(host);
}

// Each once, as the apps share the recipes' folder.
std::string Views::unsaved(const std::filesystem::path &folder) const {
  std::vector<CommandLog::Mark> marks;
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed)
      marks.push_back({hosted->name, hosted->marked});
  return _port.session().after(marks, folder);
}

std::vector<std::pair<std::string, std::filesystem::path>> Views::roots() const {
  std::vector<std::pair<std::string, std::filesystem::path>> roots;
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (const std::filesystem::path root = Manifest::root(hosted->current());
        !hosted->removed && std::ranges::none_of(roots, [&](const auto &named) {
          return named.second == root;
        }))
      roots.emplace_back(hosted->current().name, root);
  return roots;
}

// By index: a manifest read again may host more views, which come after.
void Views::reload() {
  for (std::size_t index = 0; index < _hosted.size(); ++index)
    if (!_hosted[index]->removed)
      reload(*_hosted[index]);
}

// A change waiting for the next frame, as an edit does, so whoever builds the frame
// sees the view first: the window must be open before a draw's pipeline is made.
void Views::reload(Hosted &hosted) {
  if (const auto time = stamp(hosted.current().file); time != hosted.read) {
    try {
      reread(hosted);
      hosted.read = time;
      return;
    } catch (const std::exception &failure) {
      _wiring.log.write(Level::error,
                        Tag::mod,
                        std::format("{}; the running graph stays", failure.what()));
    }
  }
  if (hosted.edited)
    return;
  // The same view, so the rebuild takes the new modules and SPIR-V and keeps the rest; a
  // node's folder, or a used recipe's view.vlp, may have changed with them.
  View view = *hosted.view;
  const std::vector<std::string> notes = Manifest::refresh(view);
  try {
    hosted.edited_flat = flat_of(view, _recipes);
    hosted.edited = std::make_unique<View>(std::move(view));
    note(_wiring.log, *hosted.edited, Level::info, notes);
  } catch (const std::exception &failure) {
    _wiring.log.write(Level::error,
                      Tag::mod,
                      std::format("{}; the running graph stays", failure.what()));
    hosted.edited_flat = std::make_unique<View>(*hosted.flat);
    hosted.edited = std::make_unique<View>(*hosted.view);
  }
}

// The manifest as it is now, and over it the edits since its file last held the view, as
// a restart replays them (V08), so a view a command hosted, as the ide the arguments
// name, runs on, and no edit is lost; their mark stays, as the file holds them no more
// than before. Hosting follows once they all ran, so no view goes and comes back anew
// between two of them. Throws, the view left as it runs, for a manifest that does not
// load, or that names a view which cannot be hosted.
void Views::reread(Hosted &hosted) {
  const CommandLog::Mark mark{hosted.name, hosted.marked};
  const std::string unsaved = _port.session().after(std::span(&mark, 1), {});
  auto view = std::make_unique<View>(read(_wiring.log, hosted.current().file));
  auto flat = flat_of(*view, _recipes);
  std::unique_ptr<View> was = std::exchange(hosted.edited, std::move(view));
  std::unique_ptr<View> was_flat = std::exchange(hosted.edited_flat, std::move(flat));
  try {
    for (const std::string &refused : _port.replay(unsaved))
      _wiring.log.write(Level::warn,
                        Tag::mod,
                        std::format("{} read again: {}, so it is not replayed",
                                    hosted.current().file.string(),
                                    refused));
    follow(hosted.name, hosted.current());
  } catch (...) {
    hosted.edited = std::move(was);
    hosted.edited_flat = std::move(was_flat);
    throw;
  }
}

const View *Views::find(std::string_view name) {
  const Hosted *const found = hosted(name);
  return found ? &found->current() : nullptr;
}

const View *Views::unfolded(std::string_view name) {
  const Hosted *const found = hosted(name);
  return found ? &found->next() : nullptr;
}

std::span<const Cost> Views::costs(std::string_view name) {
  const Hosted *const found = hosted(name);
  return found && found->schedule ? found->schedule->costs() : std::span<const Cost>();
}

std::span<const NodePort> Views::ports(std::string_view name) {
  const Hosted *const found = hosted(name);
  return found && found->schedule ? found->schedule->ports() : std::span<const NodePort>();
}

// An edit that leaves the view unable to unfold, or naming a view that cannot be hosted,
// is refused, and changes nothing.
void Views::replace(std::string_view name, View view) {
  Hosted *const found = hosted(name);
  if (!found)
    throw std::runtime_error(std::format("no view is named {}", name));
  const std::vector<std::string> notes = Manifest::refresh(view);
  auto flat = flat_of(view, _recipes);
  if (!_port.replaying())
    follow(found->name, view);
  found->edited_flat = std::move(flat);
  found->edited = std::make_unique<View>(std::move(view));
  note(_wiring.log, *found->edited, Level::info, notes);
}

std::optional<std::string> Views::host_of(std::string_view name) {
  const Hosted *const found = hosted(name);
  if (!found || found->name.empty())
    return std::nullopt;
  return found->parent;
}

void Views::command(Call &call) {
  if (call.is(_save)) {
    Hosted *const found = hosted(_port.addressed());
    if (!found)
      throw std::runtime_error("no view to save");
    save(*found);
    call.reply(std::format("saved {}{}",
                           found->current()
                               .file.lexically_proximate(std::filesystem::current_path())
                               .generic_string(),
                           project(found->current()) ? ", the project kept as it is now"
                                                     : ""));
  } else if (call.is(_list)) {
    for (const Child &child : call.view().children)
      call.reply(std::format("{} {}", child.name, child.file.string()));
  } else if (call.is(_clear)) {
    Hosted *const found = hosted(_port.addressed());
    const std::string_view port = call.arguments().front();
    const std::size_t dot = port.rfind('.');
    if (!found || !found->schedule)
      throw std::runtime_error("no view runs to clear an image in");
    if (dot == std::string_view::npos)
      throw std::runtime_error(std::format("{} is no port: node.port", port));
    found->schedule->clear_image(port.substr(0, dot), port.substr(dot + 1));
  }
}

// The host has the empty name, which no argument can give.
Views::Hosted *Views::hosted(std::string_view name) const {
  const auto found =
      std::ranges::find_if(_hosted, [&](const std::unique_ptr<Hosted> &hosted) {
        return !hosted->removed && hosted->name == name;
      });
  return found == _hosted.end() ? nullptr : found->get();
}

// A command addresses a hosted view by its name, so no two share one. The build tree
// mirrors each view by its folder's name, so two views running may share that only when
// they build from one folder, as the apps do from the recipes'.
void Views::host(const Child &child, const std::string &parent) {
  if (hosted(child.name))
    throw std::runtime_error(
        std::format("a hosted view is named {} already", child.name));
  std::error_code missing;
  const bool exists = std::filesystem::exists(child.file, missing);
  auto view = std::make_unique<View>(exists ? read(_wiring.log, child.file)
                                            : Manifest::empty(child.file));
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed && hosted->current().name == view->name &&
        Manifest::root(hosted->current()) != Manifest::root(*view))
      throw std::runtime_error(std::format(
          "child {}: {} builds as the view {}, as {} does from another folder; the "
          "build tree holds one view of each name",
          child.name,
          child.file.string(),
          view->name,
          hosted->current().file.string()));
  auto flat = flat_of(*view, _recipes);
  if (!exists)
    _wiring.log.write(
        Level::info,
        Tag::run,
        std::format(
            "child {}: no view at {} yet, so it starts empty; view save writes it",
            child.name,
            child.file.string()));
  Kept kept = kept_of(_port.files(), *view);
  Hosted &hosted = *_hosted.emplace_back(
      std::make_unique<Hosted>(Hosted{.name = child.name,
                                      .parent = parent,
                                      .edited = std::move(view),
                                      .edited_flat = std::move(flat),
                                      .read = stamp(child.file),
                                      .marked = _port.session().size(),
                                      .kept = std::move(kept)}));
  follow(hosted.name, hosted.current());
}

// The views a manifest names, hosted, and the views it no longer names taken out, with
// the views they host. A view that cannot be hosted leaves hosting as it was, and
// throws naming why.
void Views::follow(const std::string &host, const View &view) {
  const std::size_t before = _hosted.size();
  try {
    for (const Child &child : view.children) {
      const Hosted *const found = hosted(child.name);
      if (!found) {
        Views::host(child, host);
        continue;
      }
      if (found->parent != host)
        throw std::runtime_error(
            std::format("a hosted view is named {} already", child.name));
      if (found->current().file != child.file)
        throw std::runtime_error(
            std::format("child {} is hosted from {}; view close {} first",
                        child.name,
                        found->current().file.string(),
                        child.name));
    }
  } catch (const std::exception &) {
    // None of them has a schedule yet, so they go at once.
    _hosted.erase(_hosted.begin() + static_cast<std::ptrdiff_t>(before), _hosted.end());
    throw;
  }
  const std::string current(_port.current()); // where lines went before any goes
  std::vector<std::string> gone;
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed && hosted->parent == host && !hosted->name.empty() &&
        std::ranges::find(view.children, hosted->name, &Child::name) ==
            view.children.end()) {
      unhost(*hosted);
      gone.push_back(hosted->name);
    }
  // A view beside one that goes lets go of its window, as its host does on view close,
  // so nothing samples an image no view draws into. By index, as a replace may host.
  for (const std::string &name : gone)
    for (std::size_t index = 0; index < _hosted.size(); ++index) {
      const Hosted &beside = *_hosted[index];
      const auto from_it = [&](const Connection &connection) {
        return connection.from.view() && connection.from.node == name;
      };
      if (beside.removed || beside.parent != host || beside.name.empty() ||
          std::ranges::none_of(beside.current().connections, from_it))
        continue;
      View kept = beside.current();
      std::erase_if(kept.connections, from_it);
      replace(beside.name, std::move(kept));
    }
  // Lines that went to a view now gone go on to a project hosted beside it, the next
  // hosted after it, else the one before, as a closed tab gives way to its neighbour; to
  // an app, as the cli or the ide, only once no project is left, as Commands::current
  // then falls back to the views that held it.
  if (gone.empty() || hosted(current))
    return;
  const auto open_project = [](const std::unique_ptr<Hosted> &hosted) {
    return !hosted->removed && project(hosted->current());
  };
  const auto closed = std::ranges::find_if(_hosted, [&](const auto &hosted) {
    return hosted->removed && hosted->name == current;
  });
  auto next = std::find_if(closed, _hosted.end(), open_project);
  if (next == _hosted.end()) {
    const auto earlier =
        std::find_if(std::make_reverse_iterator(closed), _hosted.rend(), open_project);
    if (earlier == _hosted.rend())
      return;
    next = std::prev(earlier.base());
  }
  _port.send(std::format("view switch {}", (*next)->name));
}

void Views::unhost(Hosted &hosted) {
  hosted.removed = true;
  if (hosted.schedule)
    hosted.schedule->retire();
  for (const std::unique_ptr<Hosted> &other : _hosted)
    if (!other->removed && other->parent == hosted.name)
      unhost(*other);
}

void Views::save(Hosted &hosted) {
  write(hosted);
  hosted.kept = kept_of(_port.files(), hosted.current());
  _wiring.log.write(Level::info,
                    Tag::run,
                    std::format("view save: {}", hosted.current().file.string()));
}

// The file holds the view, so a restart replays none of its edits so far, and a read
// again finds it unchanged. A manifest changed since vulpen read or saved it holds what
// the view does not, which a write would silently lose.
void Views::write(Hosted &hosted) {
  const View &view = hosted.current();
  std::error_code missing;
  if (std::filesystem::exists(view.file, missing) && stamp(view.file) != hosted.read)
    throw std::runtime_error(
        std::format("{} changed since vulpen read it, and a save would lose that change; "
                    "move the file aside to save over it",
                    view.file.string()));
  Manifest::save(view);
  hosted.read = stamp(view.file);
  hosted.marked = _port.session().size();
}

// A project's own, never an app's or the library's, whose views the GUIs edit every
// frame (their parts' params), and whose files the library keeps.
void Views::write(std::string_view name) {
  Hosted *const found = hosted(name);
  if (!found || !project(found->current()))
    return;
  try {
    write(*found);
  } catch (const std::runtime_error &failure) {
    _wiring.log.write(Level::error, Tag::run, failure.what());
  }
}

std::vector<std::string> Views::names() {
  std::vector<std::string> names;
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed)
      names.push_back(hosted->name);
  return names;
}

std::vector<std::string> Views::unkept(std::string_view name) {
  const Hosted *const found = hosted(name);
  if (!found || !project(found->current()))
    return {};
  return differences(found->kept, kept_of(_port.files(), found->current()));
}

void Views::put_back(std::string_view name) {
  if (const Hosted *const found = hosted(name); found && project(found->current()))
    VP::put_back(_port.files(), found->current().file.parent_path(), found->kept);
}

} // namespace VP
