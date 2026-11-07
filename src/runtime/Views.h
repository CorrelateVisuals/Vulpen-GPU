#pragma once

#include "runtime/Commands.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VP {

class Log;
class Schedule;
struct Child;
struct Offscreen;
struct Pass;
struct Recipes;
struct Wiring;

// Every view this process runs: the one it started with and the views it hosts (V03),
// each with its schedule. A view's manifest names the views it hosts, so hosting follows
// its edits and its manifest; a command reaches a hosted view by its name.
class Views final : public ViewLookup, public CommandHandler {
public:
  // Builds the schedule of the view vulpen started with, hosts the views it names, and
  // registers view save, view list and image clear. recipes: what each view's recipes
  // unfold from, borrowed (A01).
  Views(const Wiring &wiring, View view, Commands &commands, Recipes &recipes);
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
  // Whether a node of a view said it read nothing at an input as the frame before
  // cooked (Cook::unread), by the view's name, the node's and the port's.
  using Unread = std::function<bool(std::string_view view,
                                    std::string_view node,
                                    std::string_view port)>;
  // Before a frame, after rebuild: which views rest, none of their nodes cooking,
  // dispatching or drawing, their windows keeping what they drew last. A view rests while
  // a view beside it presents views, as the ide presents the projects, and no connection
  // taking its window was read the frame before, as a project no deck shows, or one
  // whose every picture had no room, in a hidden panel; or while the view hosting it
  // rests. One with no such view beside it, as a project the cli alone hosts, never rests
  // (V07). A view that shows an image in its window's place (view show) runs only what
  // that image needs, and the views taking its window sample it there; rebuilt: the
  // schedules are new, so each takes that again. True when what runs changed, so the
  // passes are gathered again.
  bool pull(const Unread &unread, bool rebuilt);
  // The folders the live scan watches, each view's, by the name the build tree mirrors
  // the view by, which names its target.
  std::vector<std::pair<std::string, std::filesystem::path>> roots() const;
  // What the views' manifests do not hold yet: each hosted view's edits since it was
  // hosted or saved, in the order they ran, a line each, a file named from
  // the folder, as the log names it. Replayed on the same manifests, they rebuild the
  // views as they are (V08), as a restart does.
  std::string unsaved(const std::filesystem::path &folder) const;
  // After a live build: the next frame's schedules take what it rebuilt, and each node
  // the files its folder holds. A manifest is read again only when it changed since it
  // was read, and the edits since its file last held the view are replayed over it, so
  // they stay; a running graph stays when its manifest does not load.
  void reload();
  // After a line made, removed or moved files: each node lists the files its folder holds
  // now, and a view whose lists changed rebuilds at the next frame, so what the graph,
  // Tab and ls name is there, in a release build too, which has no live build to reload.
  void relist();

private:
  struct Hosted;
  // A line a person ran, which undo takes back and redo runs again, in Views.cpp.
  struct Step;
  // The file port lines reach files through, which keeps each change of a step.
  class Recorded;

  const View *find(std::string_view name) override;
  const View *unfolded(std::string_view name) override;
  std::span<const Cost> costs(std::string_view name) override;
  std::span<const NodePort> ports(std::string_view name) override;
  void write(std::string_view name) override;
  std::vector<std::string> names() override;
  std::vector<std::string> unkept(std::string_view name) override;
  void put_back(std::string_view name) override;
  void replace(std::string_view name, View view) override;
  std::optional<std::string> host_of(std::string_view name) override;
  void begin_step(std::string_view line) override;
  void end_step(std::optional<std::size_t> group) override;
  FilePort &files() override;
  std::size_t done() const override;
  void command(Call &call) override;
  Hosted *hosted(std::string_view name) const;
  // The view whose connections take the view's window: the view hosting it, or one
  // beside it, hosted by the same view; null for none.
  const Hosted *taker(const Hosted &hosted) const;
  // Whether the view takes the window of a view beside it, as a GUI presents a project.
  bool presents(const Hosted &hosted) const;
  bool rests(const Hosted &hosted, const Unread &unread) const;
  bool show(Hosted &hosted, bool rebuilt);
  void show_image(Call &call);
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
  void write(Hosted &hosted);
  void reload(Hosted &hosted);
  void reread(Hosted &hosted);
  void undo(Call &call, bool again);
  void check(const Step &step, bool again) const;
  void take_files_back(const Step &step);
  void bring_files_again(const Step &step);
  void remove_tree(const std::filesystem::path &path);
  void restore_views(const Step &step, bool again);
  bool kept_by_view(const Step &step, const std::filesystem::path &file, bool again) const;

  const Wiring &_wiring;
  Commands &_port; // which says the view a command addresses, and the current view
  Recipes &_recipes;
  std::vector<std::unique_ptr<Hosted>> _hosted; // the host first
  std::vector<Schedule *> _schedules;           // by _hosted, once built
  const Hosted *_owner = nullptr; // the window's, as the last rebuild chose it
  std::unique_ptr<Recorded> _recorded; // over the command port's files
  std::unique_ptr<Step> _step;         // the line a person runs now; null for none
  std::vector<Step> _done;             // the steps undo takes back, the newest last
  std::vector<Step> _undone;           // those it took back, which redo runs again
  // Registered once the views the host names are hosted, so a constructor that throws
  // leaves no command to a handler that is gone.
  Command _save;
  Command _list;
  Command _show;
  Command _clear;
  Command _undo;
  Command _redo;
};

} // namespace VP
