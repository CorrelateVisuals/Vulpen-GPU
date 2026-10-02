#include "runtime/Views.h"

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <format>
#include <stdexcept>

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

Views::Views(const Wiring &wiring, View view, Commands &commands)
    : _wiring(wiring), _view(std::make_unique<View>(std::move(view))),
      _flat(unfolded(*_view)), _read(stamp(_view->file)),
      _schedule(std::make_unique<Schedule>(wiring, *_flat)),
      _save(commands.add("view save",
                         "writes the view over its manifest, keeping the comments in it",
                         *this,
                         Primitive::yes)) {}

Views::~Views() = default;

const View &Views::view() const {
  return _edited ? *_edited_flat : *_flat;
}

// One rebuild for all the changes since the last frame, so a script is checked as a
// whole, and a replay costs about what a load does.
Schedule &Views::schedule() {
  if (_edited)
    rebuild();
  return *_schedule;
}

// A change waiting for the next frame, as an edit does, so whoever builds the frame
// sees the view first: the window must be open before a draw's pipeline is made.
void Views::reload() {
  if (const auto time = stamp(_view->file); time != _read) {
    try {
      auto view = std::make_unique<View>(Manifest::load(_view->file));
      _edited_flat = unfolded(*view);
      _edited = std::move(view);
      _read = time;
      return;
    } catch (const std::exception &failure) {
      _wiring.log.write(Level::error,
                        Tag::mod,
                        std::format("{}; the running graph stays", failure.what()));
    }
  }
  // The same view, so the rebuild takes the new modules and SPIR-V and keeps the rest; a
  // deployed recipe's view.vlp may have changed with them.
  if (_edited)
    return;
  try {
    _edited_flat = unfolded(*_view);
  } catch (const std::exception &failure) {
    _wiring.log.write(Level::error,
                      Tag::mod,
                      std::format("{}; the running graph stays", failure.what()));
    _edited_flat = std::make_unique<View>(*_flat);
  }
  _edited = std::make_unique<View>(*_view);
}

// Hosts no other view yet.
const View *Views::find(std::string_view name) {
  return name.empty() ? (_edited ? _edited.get() : _view.get()) : nullptr;
}

// An edit that leaves the view unable to unfold is refused, and changes nothing.
void Views::replace(std::string_view, View view) {
  _edited_flat = unfolded(view);
  _edited = std::make_unique<View>(std::move(view));
}

// A manifest changed since vulpen read or saved it holds what the view does not, which a
// save would silently lose.
void Views::command(Call &call) {
  if (!call.is(_save))
    return;
  const View &view = *find({});
  std::error_code missing;
  if (std::filesystem::exists(view.file, missing) && stamp(view.file) != _read)
    throw std::runtime_error(
        std::format("{} changed since vulpen read it, and a save would lose that change; "
                    "move the file aside to save over it",
                    view.file.string()));
  Manifest::save(view);
  _read = stamp(view.file);
  _wiring.log.write(
      Level::info, Tag::run, std::format("view save: {}", view.file.string()));
}

// The new schedule takes from the old one, which still reads the old view, so the old
// view goes last.
void Views::rebuild() {
  _schedule = std::make_unique<Schedule>(_wiring, *_edited_flat, _schedule.get());
  _view = std::move(_edited);
  _flat = std::move(_edited_flat);
}

} // namespace VP
