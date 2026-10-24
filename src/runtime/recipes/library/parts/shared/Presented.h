#pragma once

#include "runtime/View.h"

#include <algorithm>
#include <ranges>
#include <string_view>

namespace {

// Which project a GUI beside the projects presents, so the modes part, which switches the
// window to it, and the tiles part, which connects it and shows it, agree (RV09).

// The connection that presents a view, from the window of the view it shows.
constexpr std::string_view presenting = "present";

// The view presented, as the view's connections are; empty for none.
inline std::string_view shown_by(const VP::View &view) {
  const auto made =
      std::ranges::find(view.connections, presenting, &VP::Connection::name);
  return made != view.connections.end() && made->from.view()
             ? std::string_view(made->from.node)
             : std::string_view();
}

// Whether the view named is the current one or holds it, as far down as views host
// views; view_of finds a view by its name.
template <class Find>
bool holds(const Find &view_of, std::string_view name, std::string_view current) {
  if (name == current)
    return true;
  const VP::View *const view = view_of(name);
  return view && std::ranges::any_of(view->children, [&](const VP::Child &child) {
           return holds(view_of, child.name, current);
         });
}

// The projects a GUI presents: the views the view vulpen started with hosts, but the
// GUI's own, so those beside it, or, run as that view, those it hosts.
inline auto projects(const VP::View &top, std::string_view own) {
  return top.children |
         std::views::filter([own](const VP::Child &child) { return child.name != own; });
}

// The project that is the current view or holds it, so a nested view shows through its
// project; empty while the current view is none of them.
template <class Find, class Projects>
std::string_view project_of(const Find &view_of,
                            Projects &&projects,
                            std::string_view current) {
  for (const VP::Child &child : projects)
    if (!current.empty() && holds(view_of, child.name, current))
      return child.name;
  return {};
}

// The project to present: the current view's project; at the first frame, while none is
// presented, the one hosted last, so a run hosting projects shows one at once; else the
// one presented, while the current view is none of them, as the GUI's own or the cli.
// It points into the views, so it is copied before a line replaces them.
template <class Find>
std::string_view wanted(const Find &view_of,
                        const VP::View &top,
                        std::string_view own,
                        std::string_view current,
                        std::string_view presented,
                        bool started) {
  auto hosted = projects(top, own);
  const std::string_view project = project_of(view_of, hosted, current);
  if (!project.empty() || started || !presented.empty())
    return !project.empty() ? project : presented;
  std::string_view newest;
  for (const VP::Child &child : hosted)
    newest = child.name;
  return newest;
}

} // namespace
