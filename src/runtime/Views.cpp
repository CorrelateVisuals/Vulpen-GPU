#include "runtime/Views.h"

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>

namespace VP {

namespace {

// When the file was last written. One that is gone reads as never written, so the next
// build tries it again.
std::filesystem::file_time_type stamp(const std::filesystem::path &file) {
  std::error_code gone;
  return std::filesystem::last_write_time(file, gone);
}

// Throws naming what keeps it from running: a recipe the library lacks, or a cycle that
// runs through the nodes of a recipe a node uses, which no single edit could show.
std::unique_ptr<View> unfolded(const View &view) {
  auto flat = std::make_unique<View>(Manifest::flatten(view));
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
  std::unique_ptr<Schedule> schedule;   // null until the frame after it was hosted
  // Where its schedule's draws into its window go.
  Schedule::Into into = Schedule::Into::window;
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
Views::Views(const Wiring &wiring, View view, Commands &commands)
    : _wiring(wiring), _port(commands), _hosted([&] {
        std::vector<std::unique_ptr<Hosted>> hosted;
        Hosted &host = *hosted.emplace_back(std::make_unique<Hosted>());
        host.read = stamp(view.file);
        host.view = std::make_unique<View>(std::move(view));
        host.flat = unfolded(*host.view);
        host.schedule =
            std::make_unique<Schedule>(wiring, *host.flat, Schedule::Into::window);
        return hosted;
      }()),
      _schedules{_hosted.front()->schedule.get()} {
  follow({}, _hosted.front()->current());
  _save = commands.add("view save",
                       "writes the view over its manifest, keeping the comments in it",
                       *this,
                       Primitive::yes);
  _list = commands.add("child list",
                       "lists the hosted views, with their files, in the order hosted",
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
  return std::ranges::any_of(_hosted, [&](const std::unique_ptr<Hosted> &hosted) {
    return !hosted->removed && !target(*hosted) && Schedule::shows(hosted->next());
  });
}

// One rebuild for all the changes since the last frame, so a script is checked as a
// whole, and a replay costs about what a load does. The new schedule takes from the old
// one, which still reads the old view, so the old view goes last. A view comes after the
// view hosting it, so where its draws go follows its host's view as rebuilt.
bool Views::rebuild() {
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
    const Schedule::Into into =
        target(hosted) ? Schedule::Into::image : Schedule::Into::window;
    if (hosted.edited) {
      const bool added = !hosted.schedule;
      hosted.schedule = std::make_unique<Schedule>(
          _wiring, *hosted.edited_flat, into, hosted.schedule.get());
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
          _wiring, *hosted.flat, into, hosted.schedule.get());
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

// A view comes after the view hosting it, so going back from the last, each view's work
// comes before its host's.
void Views::gather(std::vector<Pass> &passes) const {
  const auto drawing = [&](const Hosted *into) {
    return _hosted | std::views::filter([this, into](const std::unique_ptr<Hosted> &at) {
             return !at->removed && at->schedule && target(*at) == into;
           });
  };
  for (const std::unique_ptr<Hosted> &hosted : _hosted | std::views::reverse) {
    if (hosted->removed || !hosted->schedule)
      continue;
    hosted->schedule->work(passes);
    if (target(*hosted) != hosted.get())
      continue;
    // Its host's schedule made the image as it made the connection that takes it.
    if (const Offscreen *const image =
            this->hosted(hosted->parent)->schedule->window_of(hosted->name))
      for (const std::unique_ptr<Hosted> &into : drawing(hosted.get()))
        into->schedule->draws(passes, image);
  }
  for (const std::unique_ptr<Hosted> &into : drawing(nullptr))
    into->schedule->draws(passes, nullptr);
}

const Views::Hosted *Views::target(const Hosted &hosted) const {
  if (hosted.name.empty())
    return nullptr;
  const Hosted *const host = this->hosted(hosted.parent);
  if (!host)
    return nullptr;
  if (std::ranges::any_of(host->next().connections, [&](const Connection &connection) {
        return connection.from.view() && connection.from.node == hosted.name;
      }))
    return &hosted;
  return target(*host);
}

std::vector<std::filesystem::path> Views::roots() const {
  std::vector<std::filesystem::path> roots;
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed)
      roots.push_back(Manifest::root(hosted->current()));
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
      auto view = std::make_unique<View>(read(_wiring.log, hosted.current().file));
      auto flat = unfolded(*view);
      follow(hosted.name, *view);
      hosted.edited_flat = std::move(flat);
      hosted.edited = std::move(view);
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
    hosted.edited_flat = unfolded(view);
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

const View *Views::find(std::string_view name) {
  const Hosted *const found = hosted(name);
  return found ? &found->current() : nullptr;
}

// An edit that leaves the view unable to unfold, or naming a view that cannot be hosted,
// is refused, and changes nothing.
void Views::replace(std::string_view name, View view) {
  Hosted *const found = hosted(name);
  if (!found)
    throw std::runtime_error(std::format("no view is named {}", name));
  const std::vector<std::string> notes = Manifest::refresh(view);
  auto flat = unfolded(view);
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
  } else if (call.is(_list)) {
    for (const std::unique_ptr<Hosted> &hosted : _hosted)
      if (!hosted->removed && !hosted->name.empty())
        call.reply(std::format("{} {}", hosted->name, hosted->current().file.string()));
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
// mirrors each view by its folder's name, so two views running may not share that.
void Views::host(const Child &child, const std::string &parent) {
  if (hosted(child.name))
    throw std::runtime_error(
        std::format("a hosted view is named {} already", child.name));
  std::error_code missing;
  const bool exists = std::filesystem::exists(child.file, missing);
  auto view = std::make_unique<View>(exists ? read(_wiring.log, child.file)
                                            : Manifest::empty(child.file));
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed && hosted->current().name == view->name)
      throw std::runtime_error(std::format(
          "child {}: {} is in a folder named {}, as {} is; the build tree holds one view "
          "of each name",
          child.name,
          child.file.string(),
          view->name,
          hosted->current().file.string()));
  auto flat = unfolded(*view);
  if (!exists)
    _wiring.log.write(
        Level::info,
        Tag::run,
        std::format(
            "child {}: no view at {} yet, so it starts empty; view save writes it",
            child.name,
            child.file.string()));
  Hosted &hosted = *_hosted.emplace_back(
      std::make_unique<Hosted>(Hosted{.name = child.name,
                                      .parent = parent,
                                      .edited = std::move(view),
                                      .edited_flat = std::move(flat),
                                      .read = stamp(child.file)}));
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
            std::format("child {} is hosted from {}; child remove {} first",
                        child.name,
                        found->current().file.string(),
                        child.name));
    }
  } catch (const std::exception &) {
    // None of them has a schedule yet, so they go at once.
    _hosted.erase(_hosted.begin() + static_cast<std::ptrdiff_t>(before), _hosted.end());
    throw;
  }
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed && hosted->parent == host && !hosted->name.empty() &&
        std::ranges::find(view.children, hosted->name, &Child::name) ==
            view.children.end())
      unhost(*hosted);
}

void Views::unhost(Hosted &hosted) {
  hosted.removed = true;
  for (const std::unique_ptr<Hosted> &other : _hosted)
    if (!other->removed && other->parent == hosted.name)
      unhost(*other);
}

// A manifest changed since vulpen read or saved it holds what the view does not, which a
// save would silently lose.
void Views::save(Hosted &hosted) {
  const View &view = hosted.current();
  std::error_code missing;
  if (std::filesystem::exists(view.file, missing) && stamp(view.file) != hosted.read)
    throw std::runtime_error(
        std::format("{} changed since vulpen read it, and a save would lose that change; "
                    "move the file aside to save over it",
                    view.file.string()));
  Manifest::save(view);
  hosted.read = stamp(view.file);
  _wiring.log.write(
      Level::info, Tag::run, std::format("view save: {}", view.file.string()));
}

} // namespace VP
