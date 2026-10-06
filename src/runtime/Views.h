#pragma once

#include "runtime/Commands.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

class Log;
class Schedule;
struct Child;
struct Wiring;

// Every view this process runs: the one it started with and the views it hosts (V03),
// each with its schedule. A view's manifest names the views it hosts, so hosting follows
// its edits and its manifest; a command reaches a hosted view by its name.
class Views final : public ViewLookup, public CommandHandler {
public:
  // Builds the schedule of the view vulpen started with, hosts the views it names, and
  // registers view save, child list and image clear.
  Views(const Wiring &wiring, View view, Commands &commands);
  ~Views();
  Views(const Views &) = delete;
  Views &operator=(const Views &) = delete;

  // A manifest as a view runs it: each node with the files its folder holds now. What
  // changed since the manifest listed them, and each folder no node names, go to the log.
  static View read(const Log &log, const std::filesystem::path &file);
  // The view vulpen started with, as the changes so far left it.
  const View &view() const;
  // Whether a node of any view draws, as the changes so far left them, so the next
  // frame needs a window.
  bool draws() const;
  // Before a frame: rebuilds the schedule of each view that changed since the last, and
  // drops those of the views no longer hosted. True when any schedule changed.
  bool rebuild();
  // The schedules a frame runs: the host's first, then the hosted views' in the order
  // they were hosted.
  std::span<Schedule *const> schedules() const;
  // The folders the live scan watches: each view's.
  std::vector<std::filesystem::path> roots() const;
  // After a live build: the next frame's schedules take what it rebuilt, and each node
  // the files its folder holds. A manifest is read again only when it changed since it
  // was read, so the edits since then stay; a running graph stays when its manifest does
  // not load.
  void reload();

private:
  struct Hosted;

  const View *find(std::string_view name) override;
  void replace(std::string_view name, View view) override;
  std::optional<std::string> host_of(std::string_view name) override;
  void command(Call &call) override;
  Hosted *hosted(std::string_view name) const;
  void host(const Child &child, const std::string &parent);
  void follow(const std::string &host, const View &view);
  void unhost(Hosted &hosted);
  void save(Hosted &hosted);
  void reload(Hosted &hosted);

  const Wiring &_wiring;
  const Commands &_port;                        // which says the view a command addresses
  std::vector<std::unique_ptr<Hosted>> _hosted; // the host first
  std::vector<Schedule *> _schedules;           // by _hosted, once built
  // Registered once the views the host names are hosted, so a constructor that throws
  // leaves no command to a handler that is gone.
  Command _save;
  Command _list;
  Command _clear;
};

} // namespace VP
