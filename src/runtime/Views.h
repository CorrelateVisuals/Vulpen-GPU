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
  // Builds the schedule of the view vulpen started with, and registers view save.
  Views(const Wiring &wiring, View view, Commands &commands);
  ~Views();
  Views(const Views &) = delete;
  Views &operator=(const Views &) = delete;

  // The view as the changes so far left it, deploys unfolded, which the next frame's
  // schedule runs.
  const View &view() const;
  // The schedule a frame runs, rebuilt first if the view changed since.
  Schedule &schedule();
  // After a live build: the next frame's schedule takes what it rebuilt. The manifest is
  // read again only when it changed since it was read, so the edits since then stay; the
  // running graph stays when it does not load.
  void reload();

private:
  const View *find(std::string_view name) override;
  void replace(std::string_view name, View view) override;
  void command(Call &call) override;
  void rebuild();

  const Wiring &_wiring;
  // The view as its manifest says, which edits and save work on, and with its deploys
  // unfolded, which the schedule runs and so outlives.
  std::unique_ptr<View> _view;
  std::unique_ptr<View> _flat;
  // What commands made of the view since, both ways; null when nothing.
  std::unique_ptr<View> _edited;
  std::unique_ptr<View> _edited_flat;
  std::filesystem::file_time_type _read; // the manifest's, when it was last read or saved
  std::unique_ptr<Schedule> _schedule;
  const Command _save;
};

} // namespace VP
