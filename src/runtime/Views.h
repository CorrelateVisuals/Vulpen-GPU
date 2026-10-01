#pragma once

#include "runtime/Commands.h"

#include <filesystem>
#include <memory>
#include <string_view>

namespace VP {

class Schedule;
struct Wiring;

// Every view this process runs: the one it started with and the views it hosts (V03),
// each with its schedule. Hosting is session state, so the log keeps it and the host's
// manifest does not; a command reaches a hosted view by its name.
class Views final : public ViewLookup, public CommandHandler {
public:
  // Builds the schedule of the view vulpen started with.
  Views(const Wiring &wiring, View view);
  ~Views();
  Views(const Views &) = delete;
  Views &operator=(const Views &) = delete;

  // The schedule a frame runs.
  Schedule &schedule();
  // After a live build. The manifest is read again only when it changed since it was
  // read, so the edits since then stay; the running graph stays when it does not load.
  void reload();

private:
  const View *find(std::string_view name) override;
  void replace(std::string_view name, View view) override;
  void command(Call &call) override;
  // Null keeps the view and builds its schedule anew.
  void rebuild(std::unique_ptr<View> view);

  const Wiring &_wiring;
  std::unique_ptr<View> _view; // what the schedule runs, so it outlives the schedule
  std::filesystem::file_time_type _read; // the manifest's, when it was last read
  std::unique_ptr<Schedule> _schedule;
};

} // namespace VP
