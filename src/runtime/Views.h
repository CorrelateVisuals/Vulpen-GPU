#pragma once

#include "runtime/Commands.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace VP {

class Schedule;
struct Wiring;

// Every view this process runs: the one it started with and the views it hosts (V03),
// each with its schedule. Hosting is session state, so the log keeps it and the host's
// manifest does not; a command reaches a hosted view by its name.
class Views final : public ViewLookup, public CommandHandler {
public:
  // Builds the schedule of the view vulpen started with, and registers view save and the
  // child commands.
  Views(const Wiring &wiring, View view, Commands &commands);
  ~Views();
  Views(const Views &) = delete;
  Views &operator=(const Views &) = delete;

  // The view vulpen started with, as the changes so far left it.
  const View &view() const;
  // Whether a node of any view draws, as the changes so far left them, so the next
  // frame needs a window.
  bool draws() const;
  // Before a frame: rebuilds the schedule of each view that changed since the last, and
  // drops those of the views no longer hosted. True when any schedule changed.
  bool rebuild();
  // The schedules a frame runs: the host's first, then the hosted views' in the order
  // they were added.
  std::span<Schedule *const> schedules() const;
  // The folders the live scan watches: each view's.
  std::vector<std::filesystem::path> roots() const;
  // After a live build: the next frame's schedules take what it rebuilt. A manifest is
  // read again only when it changed since it was read, so the edits since then stay; a
  // running graph stays when its manifest does not load.
  void reload();

private:
  struct Hosted;

  const View *find(std::string_view name) override;
  void replace(std::string_view name, View view) override;
  void command(Call &call) override;
  Hosted *hosted(std::string_view name) const;
  void host(std::string_view name, const std::filesystem::path &file);
  void save(Hosted &hosted);
  void reload(Hosted &hosted);

  const Wiring &_wiring;
  const Commands &_port;                        // which says the view a command addresses
  std::vector<std::unique_ptr<Hosted>> _hosted; // the host first
  std::vector<Schedule *> _schedules;           // by _hosted, once built
  const Command _save;
  const Command _add;
  const Command _remove;
  const Command _list;
};

} // namespace VP
