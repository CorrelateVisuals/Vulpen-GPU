#pragma once

#include "runtime/View.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Which project a GUI beside the projects presents, and which deck shows which, so the
// modes part, which switches the window to it, the panes part, which connects the
// projects' windows, the deck part, which shows one of those it holds, and the docking
// part, which moves them between decks, agree (RV09).

// The connection that presents a view, from the window of the view it shows.
constexpr std::string_view presenting = "present";
// The deck part's recipe, and its params: the views it holds, by name, joined by commas,
// in the order its tabs show them, and the one it shows.
constexpr std::string_view deck_recipe = "deck";
constexpr std::string_view holds_param = "holds";
constexpr std::string_view shown_param = "shown";

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

// A param of a node, as the view holds it; empty for none.
inline std::string_view param_value(const VP::Node &node, std::string_view key) {
  const auto found = std::ranges::find(node.params, key, &VP::Param::key);
  return found == node.params.end() ? std::string_view() : std::string_view(found->value);
}

// The names a param joins by commas, in order, none empty.
inline std::vector<std::string> split_names(std::string_view joined) {
  std::vector<std::string> names;
  for (const auto word : std::views::split(joined, ','))
    if (!std::ranges::empty(word))
      names.emplace_back(word.begin(), word.end());
  return names;
}

inline std::string joined_names(const std::vector<std::string> &names) {
  std::string text;
  for (const std::string &name : names)
    text += (text.empty() ? "" : ",") + name;
  return text;
}

// The view a deck shows of those it holds that hosts takes: the one presented while it
// holds that one, else the one its shown param names, else the first; empty for none.
template <class Hosts>
std::string deck_shows(const VP::Node &deck,
                       std::string_view presented,
                       const Hosts &hosts) {
  const std::vector<std::string> held = split_names(param_value(deck, holds_param));
  const auto has = [&](std::string_view name) {
    return !name.empty() && hosts(name) && std::ranges::find(held, name) != held.end();
  };
  if (has(presented))
    return std::string(presented);
  if (const std::string_view shown = param_value(deck, shown_param); has(shown))
    return std::string(shown);
  const auto first = std::ranges::find_if(held, hosts);
  return first == held.end() ? std::string() : *first;
}

} // namespace
