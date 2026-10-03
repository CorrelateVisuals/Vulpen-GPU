#include "runtime/Views.h"

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <algorithm>
#include <format>
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

// Throws naming what keeps it from running: a deploy that does not unfold, or a cycle
// that runs through a deploy's nodes, which no single edit could show.
std::unique_ptr<View> unfolded(const View &view) {
  auto flat = std::make_unique<View>(Manifest::flatten(view));
  Schedule::order(*flat);
  return flat;
}

} // namespace

// One view and its schedule. Edits and save work on the view as its manifest says, and
// the schedule runs it with its deploys unfolded, so it outlives the schedule.
struct Views::Hosted {
  std::string name; // empty for the view vulpen started with
  std::unique_ptr<View> view;
  std::unique_ptr<View> flat;
  // What commands made of the view since, both ways; null when nothing.
  std::unique_ptr<View> edited;
  std::unique_ptr<View> edited_flat;
  std::filesystem::file_time_type read; // the manifest's, when last read or saved
  std::unique_ptr<Schedule> schedule;   // null until the frame after it was added
  bool removed = false;                 // its schedule goes before the next frame

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
        host.schedule = std::make_unique<Schedule>(wiring, *host.flat);
        return hosted;
      }()),
      _schedules{_hosted.front()->schedule.get()},
      _save(commands.add("view save",
                         "writes the view over its manifest, keeping the comments in it",
                         *this,
                         Primitive::yes)),
      _add(commands.add("child add <name> <file>",
                        "hosts the view a view.vlp holds, or an empty one that view save "
                        "writes; a line `<name>: <command>` addresses it",
                        *this,
                        Primitive::yes)),
      _remove(commands.add("child remove <name>",
                           "stops hosting a view; its files stay",
                           *this,
                           Primitive::yes)),
      _list(commands.add("child list",
                         "lists the hosted views, with their files, in the order added",
                         *this)) {}

Views::~Views() = default;

const View &Views::view() const {
  return _hosted.front()->current();
}

bool Views::draws() const {
  return std::ranges::any_of(_hosted, [](const std::unique_ptr<Hosted> &hosted) {
    return !hosted->removed && Schedule::draws(hosted->next());
  });
}

// One rebuild for all the changes since the last frame, so a script is checked as a
// whole, and a replay costs about what a load does. The new schedule takes from the old
// one, which still reads the old view, so the old view goes last.
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
    if (hosted.edited) {
      const bool added = !hosted.schedule;
      hosted.schedule =
          std::make_unique<Schedule>(_wiring, *hosted.edited_flat, hosted.schedule.get());
      hosted.view = std::move(hosted.edited);
      hosted.flat = std::move(hosted.edited_flat);
      if (added)
        _wiring.log.write(Level::info,
                          Tag::run,
                          std::format("child {}: hosted from {}",
                                      hosted.name,
                                      hosted.view->file.string()));
      changed = true;
    }
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

std::vector<std::filesystem::path> Views::roots() const {
  std::vector<std::filesystem::path> roots;
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed)
      roots.push_back(Manifest::root(hosted->current()));
  return roots;
}

void Views::reload() {
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed)
      reload(*hosted);
}

// A change waiting for the next frame, as an edit does, so whoever builds the frame
// sees the view first: the window must be open before a draw's pipeline is made.
void Views::reload(Hosted &hosted) {
  if (const auto time = stamp(hosted.current().file); time != hosted.read) {
    try {
      auto view = std::make_unique<View>(Manifest::load(hosted.current().file));
      hosted.edited_flat = unfolded(*view);
      hosted.edited = std::move(view);
      hosted.read = time;
      return;
    } catch (const std::exception &failure) {
      _wiring.log.write(Level::error,
                        Tag::mod,
                        std::format("{}; the running graph stays", failure.what()));
    }
  }
  // The same view, so the rebuild takes the new modules and SPIR-V and keeps the rest; a
  // deployed recipe's view.vlp may have changed with them.
  if (hosted.edited)
    return;
  try {
    hosted.edited_flat = unfolded(*hosted.view);
  } catch (const std::exception &failure) {
    _wiring.log.write(Level::error,
                      Tag::mod,
                      std::format("{}; the running graph stays", failure.what()));
    hosted.edited_flat = std::make_unique<View>(*hosted.flat);
  }
  hosted.edited = std::make_unique<View>(*hosted.view);
}

const View *Views::find(std::string_view name) {
  const Hosted *const found = hosted(name);
  return found ? &found->current() : nullptr;
}

// An edit that leaves the view unable to unfold is refused, and changes nothing.
void Views::replace(std::string_view name, View view) {
  Hosted *const found = hosted(name);
  if (!found)
    throw std::runtime_error(std::format("no view is named {}", name));
  found->edited_flat = unfolded(view);
  found->edited = std::make_unique<View>(std::move(view));
}

void Views::command(Call &call) {
  const std::span<const std::string_view> arguments = call.arguments();
  if (call.is(_save)) {
    Hosted *const found = hosted(_port.addressed());
    if (!found)
      throw std::runtime_error("no view to save");
    save(*found);
  } else if (call.is(_add)) {
    host(arguments[0], arguments[1]);
  } else if (call.is(_remove)) {
    Hosted *const found = hosted(arguments[0]);
    if (!found)
      throw std::runtime_error(
          std::format("no view is hosted as {}; child list lists them", arguments[0]));
    found->removed = true;
  } else if (call.is(_list)) {
    for (const std::unique_ptr<Hosted> &hosted : _hosted)
      if (!hosted->removed && !hosted->name.empty())
        call.reply(std::format("{} {}", hosted->name, hosted->current().file.string()));
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

// The build tree mirrors each view by its folder's name, so two views running may not
// share one.
void Views::host(std::string_view name, const std::filesystem::path &file) {
  if (hosted(name))
    throw std::runtime_error(std::format("a hosted view is named {} already", name));
  std::error_code missing;
  const bool exists = std::filesystem::exists(file, missing);
  auto view =
      std::make_unique<View>(exists ? Manifest::load(file) : Manifest::empty(file));
  for (const std::unique_ptr<Hosted> &hosted : _hosted)
    if (!hosted->removed && hosted->current().name == view->name)
      throw std::runtime_error(std::format(
          "child {}: {} is in a folder named {}, as {} is; the build tree holds one view "
          "of each name",
          name,
          file.string(),
          view->name,
          hosted->current().file.string()));
  auto flat = unfolded(*view);
  if (!exists)
    _wiring.log.write(
        Level::info,
        Tag::run,
        std::format(
            "child {}: no view at {} yet, so it starts empty; view save writes it",
            name,
            file.string()));
  _hosted.push_back(std::make_unique<Hosted>(Hosted{.name = std::string(name),
                                                    .edited = std::move(view),
                                                    .edited_flat = std::move(flat),
                                                    .read = stamp(file)}));
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
