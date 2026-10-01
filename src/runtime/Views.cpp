#include "runtime/Views.h"

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <format>

namespace VP {

namespace {

// When the file was last written. One that is gone reads as never written, so the next
// build tries it again.
std::filesystem::file_time_type stamp(const std::filesystem::path &file) {
  std::error_code gone;
  return std::filesystem::last_write_time(file, gone);
}

} // namespace

Views::Views(const Wiring &wiring, View view)
    : _wiring(wiring), _view(std::make_unique<View>(std::move(view))),
      _read(stamp(_view->file)), _schedule(std::make_unique<Schedule>(wiring, *_view)) {}

Views::~Views() = default;

Schedule &Views::schedule() {
  return *_schedule;
}

void Views::reload() {
  std::unique_ptr<View> view;
  if (const auto time = stamp(_view->file); time != _read) {
    try {
      view = std::make_unique<View>(Manifest::load(_view->file));
      _read = time;
    } catch (const std::exception &failure) {
      _wiring.log.write(Level::error,
                        Tag::mod,
                        std::format("{}; the running graph stays", failure.what()));
    }
  }
  rebuild(std::move(view));
}

// Hosts no other view yet.
const View *Views::find(std::string_view name) {
  return name.empty() ? _view.get() : nullptr;
}

void Views::replace(std::string_view, View view) {
  rebuild(std::make_unique<View>(std::move(view)));
}

void Views::command(Call &) {}

// The new schedule takes from the old one, which still reads the old view, so the old
// view goes last.
void Views::rebuild(std::unique_ptr<View> view) {
  _schedule = std::make_unique<Schedule>(_wiring, view ? *view : *_view, _schedule.get());
  if (view)
    _view = std::move(view);
}

} // namespace VP
