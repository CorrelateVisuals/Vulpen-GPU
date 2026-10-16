#pragma once

#include "runtime/Commands.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

class Log;
class Schedule;
struct Child;
struct Offscreen;
struct Pass;
struct Wiring;

// Every view this process runs: the one it started with and the views it hosts (V03),
// each with its schedule. A view's manifest names the views it hosts, so hosting follows
// its edits and its manifest; a command reaches a hosted view by its name.
class Views final : public ViewLookup, public CommandHandler {
public:
  // Builds the schedule of the view vulpen started with, hosts the views it names, and
  // registers view save, view list and image clear.
  Views(const Wiring &wiring, View view, Commands &commands);
  ~Views();
  Views(const Views &) = delete;
  Views &operator=(const Views &) = delete;

  // A manifest as a view runs it: each node with the files its folder holds now. What
  // changed since the manifest listed them, and each folder no node names, go to the log.
  static View read(const Log &log, const std::filesystem::path &file);
  // The view vulpen started with, as the changes so far left it.
  const View &view() const;
  // Whether a node of any view draws into the window, as the changes so far left them,
  // so the next frame needs one. A view whose window a view hosting it, or one beside
  // it, takes draws into an image instead, and so do the views it hosts that draw into
  // its window; one whose host draws into the same place itself draws nowhere, and so
  // does each view the view vulpen started with hosts but the one the window shows.
  bool shows() const;
  // Before a frame: rebuilds the schedule of each view that changed since the last, or
  // whose draws into its window go elsewhere now, and drops those of the views no longer
  // hosted. True when any schedule changed.
  bool rebuild();
  // The schedules a frame cooks: the host's first, then the hosted views' in the order
  // they were hosted.
  std::span<Schedule *const> schedules() const;
  // Every view's passes, as a frame runs them: a view's work before that of the view
  // that takes its window, its host or one beside it, which may sample it; each image a
  // window renders into right after the work of the views that draw into it, all its
  // draws in one render pass; and the draws into the window last, the host's first, so a
  // hosted view draws over a host that draws nothing there itself.
  void gather(std::vector<Pass> &passes) const;
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
  // The view whose connections take the view's window: the view hosting it, or one
  // beside it, hosted by the same view; null for none.
  const Hosted *taker(const Hosted &hosted) const;
  // Whether the view takes the window of a view beside it, as a GUI presents a project.
  bool presents(const Hosted &hosted) const;
  // Whether the view draws into its window, or a view it hosts does there.
  bool draws(const Hosted &hosted) const;
  // Of the views the view vulpen started with hosts, the one the window shows while that
  // view draws nothing there itself, so the window has one owner (A01): one that
  // presents a view beside it, or has presented one, until it closes; else the current
  // view, or the view holding it, while it draws; else the first hosted that draws. A
  // view in its first frame is one of them only while no other draws, so a window none
  // drew in opens at once, and a view beside it may present it before it ever shows.
  // Null for none.
  const Hosted *owner() const;
  // The view whose window its draws into its window render into: its own, while a view
  // takes it with a connection; none, while the view hosting it draws into the same
  // place itself, which then is that view's alone (A01), or while the window shows
  // another of the views the view vulpen started with hosts; else the host's. Null for
  // the window.
  std::optional<const Hosted *> target(const Hosted &hosted, const Hosted *owner) const;
  // Why a view target sends nowhere does, for the log.
  std::string_view nowhere(const Hosted &hosted) const;
  // After the views whose windows it may sample: those it hosts, and those beside it
  // whose windows it takes.
  void gather(const Hosted &hosted,
              std::vector<Pass> &passes,
              std::vector<const Hosted *> &gathered) const;
  // The draws of each view whose draws into its window go where into's go, null for the
  // window, in the order hosted, recording into the image given, or the window for none.
  void draw_into(const Hosted *into,
                 const Offscreen *image,
                 std::vector<Pass> &passes) const;
  void host(const Child &child, const std::string &parent);
  void follow(const std::string &host, const View &view);
  void unhost(Hosted &hosted);
  void save(Hosted &hosted);
  void reload(Hosted &hosted);

  const Wiring &_wiring;
  Commands &_port; // which says the view a command addresses, and the current view
  std::vector<std::unique_ptr<Hosted>> _hosted; // the host first
  std::vector<Schedule *> _schedules;           // by _hosted, once built
  const Hosted *_owner = nullptr; // the window's, as the last rebuild chose it
  // Registered once the views the host names are hosted, so a constructor that throws
  // leaves no command to a handler that is gone.
  Command _save;
  Command _list;
  Command _clear;
};

} // namespace VP
