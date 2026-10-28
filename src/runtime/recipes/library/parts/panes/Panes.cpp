#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Presented.h"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view typed_port = "typed"; // the keys part's, which panes take too
// The connections bringing a view's window to its pane while that view is not the one
// presented, named for the pane, and what a pane's name, and its room's connection's,
// add to its view's.
constexpr std::string_view window_prefix = "window-";
constexpr std::string_view pane_suffix = "-pane";
constexpr std::string_view room_suffix = "-room";

// The last name of a node inside others, as a connection named for it takes it.
std::string_view short_name(std::string_view node) {
  return node.substr(node.rfind('.') + 1);
}

// A port of a node inside a pane, as its params name them, as image.area: the node, and
// the port.
std::pair<std::string_view, std::string_view> split_port(std::string_view inside) {
  const std::size_t dot = inside.rfind('.');
  return {inside.substr(0, dot), inside.substr(dot + 1)};
}

// A pane a view hosted beside its GUI, a node of the recipe its pane param names, as the
// still-image component: added as the view comes and removed as it goes, so what it
// holds, as a frame held still and where it is moved and zoomed, lasts while the view
// does, and the memory it takes only then, for as many views as there are. A pane takes
// its room from the deck whose holds param names its view, at that deck's port named for
// the view, at the ports its rooms param names, each a node inside the pane and a port,
// joined by commas, and the keys part's typed, which its host connects to its typed, at
// those its keys param names; a view no deck holds joins the holds of the deck holding
// the view presented before it, or else of the first deck, as view load hosts one, and a
// view closed leaves them. It connects a view's window to its pane, at the ports its
// pictures param names, while its deck shows it, and the window of the view presented
// also to the ports its to param names, as the image filling the window in perform mode,
// through the connection shared/Presented.h names: one connection a window, as a port
// joins one; the window of a view no deck shows nor presented draws nowhere. The view
// presented is the current view's project, or the one holding the current view.
class Panes final : public VP::Operator {
  // A pane as the view holds it: its node, the view and the deck its room comes from, and
  // the connection bringing it.
  struct Pane {
    std::string node;
    std::string view;
    std::string deck;
    VP::Connection room;
  };

  void bind(VP::Bind &node) override {
    _name = node.name();
    _view = node.view_name();
    _parent = _name.substr(0, std::min(_name.rfind('.'), _name.size()));
    _to = node.sets("to") ? node.param<std::string>("to") : std::string();
    std::ranges::replace(_to, ',', ' ');
    _pane = node.param<std::string>("pane");
    _rooms = split_names(node.param<std::string>("rooms"));
    _keys = node.sets("keys") ? split_names(node.param<std::string>("keys"))
                              : std::vector<std::string>();
    _pictures = split_names(node.param<std::string>("pictures"));
    if (_pane.empty() || _rooms.empty() || _pictures.empty())
      throw std::runtime_error(std::format(
          "{} needs a pane, the rooms and the pictures of it, as image.area", _name));
    if (std::ranges::any_of(node.view().connections, [&](const VP::Connection &c) {
          return std::ranges::any_of(c.to, [&](const VP::Endpoint &to) {
            return to.node == _name && to.port == typed_port;
          });
        }))
      node.input<VP_VIEW::Typed>(typed_port);
    _dirty = true; // a rebuild may have moved a view to another deck
  }

  // The holds, the panes and the windows follow in the frame, each from the view as the
  // lines so far left it, so a project's window is taken the frame it comes, and never
  // draws into the window, which a run with none has not.
  void cook(VP::Cook &frame) override {
    const VP::View &top = *frame.view("");
    const std::string_view now =
        wanted([&](std::string_view name) { return frame.view(name); },
               top,
               _view,
               frame.commands().current(),
               shown_by(*frame.view(_view)),
               _started);
    _started = true;
    if (now != _selected) {
      _previous = std::exchange(_selected, std::string(now));
      _dirty = true;
    }
    auto hosted = projects(top, _view);
    if (!std::ranges::equal(_hosted, hosted, {}, {}, &VP::Child::name)) {
      std::vector<std::string> names;
      for (const VP::Child &child : hosted)
        names.push_back(child.name);
      for (const std::string &was : _hosted)
        if (std::ranges::find(names, was) == names.end())
          _closed.push_back(was);
      _hosted = std::move(names);
      _dirty = true;
    }
    if (!std::exchange(_dirty, false))
      return;
    VP::CommandPort &commands = frame.commands();
    hold(commands, *frame.view(_view));
    _closed.clear();
    place(commands, *frame.view(_view));
    connect(commands, *frame.view(_view));
  }

  bool hosts(std::string_view view) const {
    return std::ranges::find(_hosted, view) != _hosted.end();
  }

  // Each deck lets go of the views closed since, and a view hosted that no deck holds
  // joins the deck holding the view presented before it, or else the first deck. Whether
  // it sent a line.
  bool hold(VP::CommandPort &commands, const VP::View &own) {
    struct Held {
      std::string deck;
      std::vector<std::string> views;
      bool changed = false;
    };
    std::vector<Held> decks;
    for (const VP::Node &node : own.nodes)
      if (node.recipe == deck_recipe)
        decks.push_back({node.name, split_names(param_value(node, holds_param))});
    if (decks.empty())
      return false;
    for (Held &deck : decks)
      deck.changed = std::erase_if(deck.views, [&](const std::string &view) {
                       return std::ranges::find(_closed, view) != _closed.end();
                     }) > 0;
    const auto holding = [&](std::string_view view) {
      const auto found = std::ranges::find_if(decks, [&](const Held &deck) {
        return std::ranges::find(deck.views, view) != deck.views.end();
      });
      return found == decks.end() ? nullptr : &*found;
    };
    Held *const before = holding(_previous);
    Held &active = before ? *before : decks.front();
    for (const std::string &view : _hosted)
      if (!holding(view)) {
        active.views.push_back(view);
        active.changed = true;
      }
    bool sent = false;
    for (const Held &deck : decks)
      if (deck.changed) {
        commands.send(
            deck.views.empty()
                ? std::format("{}: param unset {} {}", _view, deck.deck, holds_param)
                : std::format("{}: param set {} {} {}",
                              _view,
                              deck.deck,
                              holds_param,
                              joined_names(deck.views)));
        sent = true;
      }
    return sent;
  }

  // The panes there are: each a node of the pane recipe whose first room a deck's port
  // reaches, the view it shows named by that port.
  std::vector<Pane> panes_in(const VP::View &own) const {
    const auto [room_node, room_port] = split_port(_rooms.front());
    std::vector<Pane> panes;
    for (const VP::Node &node : own.nodes) {
      if (node.recipe != _pane)
        continue;
      const std::string reached = std::format("{}.{}", node.name, room_node);
      const auto room =
          std::ranges::find_if(own.connections, [&](const VP::Connection &c) {
            return std::ranges::any_of(c.to, [&](const VP::Endpoint &to) {
              return to.node == reached && to.port == room_port;
            });
          });
      const bool from_deck =
          room != own.connections.end() &&
          std::ranges::any_of(own.nodes, [&](const VP::Node &deck) {
            return deck.recipe == deck_recipe && deck.name == room->from.node;
          });
      panes.push_back(from_deck ? Pane{node.name, room->from.port, room->from.node, *room}
                                : Pane{.node = node.name});
    }
    return panes;
  }

  // The first deck whose holds param names the view; empty for none.
  static std::string deck_of(const VP::View &own, std::string_view view) {
    for (const VP::Node &node : own.nodes) {
      if (node.recipe != deck_recipe)
        continue;
      const std::vector<std::string> held = split_names(param_value(node, holds_param));
      if (std::ranges::find(held, view) != held.end())
        return node.name;
    }
    return {};
  }

  // A pane a view hosted that a deck holds, its room from that deck; a pane whose view
  // went, or no deck holds, goes. Whether it sent a line.
  bool place(VP::CommandPort &commands, const VP::View &own) {
    const std::vector<Pane> panes = panes_in(own);
    std::vector<std::string> going;
    // A pane, and the deck now holding its view; a view, and the deck holding it.
    std::vector<std::pair<Pane, std::string>> moving;
    std::vector<std::pair<std::string, std::string>> adding;
    for (const Pane &pane : panes) {
      const std::string deck =
          pane.view.empty() ? std::string() : deck_of(own, pane.view);
      if (!hosts(pane.view) || deck.empty())
        going.push_back(pane.node);
      else if (deck != pane.deck)
        moving.emplace_back(pane, deck);
    }
    for (const std::string &view : _hosted)
      if (const std::string deck = deck_of(own, view);
          !deck.empty() && std::ranges::none_of(panes, [&](const Pane &pane) {
            return pane.view == view;
          }))
        adding.emplace_back(view, deck);
    std::vector<std::string> taken;
    for (const auto &[view, deck] : adding)
      taken.push_back(
          free_name(own, std::format("{}.{}{}", _parent, view, pane_suffix), taken));
    const VP::Connection *const keys = keys_of(own);
    std::optional<VP::Connection> grown = keys ? std::optional(*keys) : std::nullopt;
    // The view as the frame began still has the panes going, whose ends their removal
    // takes off the keys' connection, so a pane added in the same frame, as view save as
    // closes one project and hosts its copy, connects it again without them.
    if (grown)
      std::erase_if(grown->to, [&](const VP::Endpoint &end) {
        return std::ranges::any_of(going, [&](const std::string &node) {
          return end.node == node || end.node.starts_with(node + '.');
        });
      });
    for (const std::string &node : going)
      commands.send(std::format("{}: node remove {}", _view, node));
    for (const auto &[pane, deck] : moving) {
      commands.send(std::format("{}: disconnect {}", _view, pane.room.name));
      send_connect(commands,
                   pane.room.name,
                   std::format("{}.{}", deck, pane.view),
                   pane.room.to);
    }
    for (std::size_t at = 0; at < adding.size(); ++at) {
      const std::string &node = taken[at];
      commands.send(std::format("{}: node add {} recipe={}", _view, node, _pane));
      commands.send(std::format("{}: node move {} {}", _view, node, _name));
      std::vector<VP::Endpoint> rooms;
      for (const std::string &room : _rooms) {
        const auto [inside, port] = split_port(room);
        rooms.push_back({std::format("{}.{}", node, inside), std::string(port)});
      }
      send_connect(commands,
                   std::format("{}{}", short_name(node), room_suffix),
                   std::format("{}.{}", adding[at].second, adding[at].first),
                   rooms);
      if (grown)
        for (const std::string &key : _keys) {
          const auto [inside, port] = split_port(key);
          grown->to.push_back({std::format("{}.{}", node, inside), std::string(port)});
        }
    }
    if (grown && !adding.empty()) {
      commands.send(std::format("{}: disconnect {}", _view, grown->name));
      send_connect(commands, grown->name, grown->from.text(), grown->to);
    }
    return !going.empty() || !moving.empty() || !adding.empty();
  }

  // The connection that brings the keys part's typed to it, which each pane joins.
  const VP::Connection *keys_of(const VP::View &own) const {
    const auto found =
        std::ranges::find_if(own.connections, [&](const VP::Connection &c) {
          return std::ranges::any_of(c.to, [&](const VP::Endpoint &to) {
            return to.node == _name && to.port == typed_port;
          });
        });
    return found == own.connections.end() ? nullptr : &*found;
  }

  void send_connect(VP::CommandPort &commands,
                    std::string_view name,
                    std::string_view from,
                    const std::vector<VP::Endpoint> &to) const {
    std::string line = std::format("{}: connect {} {}", _view, name, from);
    for (const VP::Endpoint &end : to)
      line += " " + end.text();
    commands.send(line);
  }

  // The name wanted, or with a number after it while a node, or a name taken, has it.
  static std::string free_name(const VP::View &own,
                               const std::string &wanted,
                               const std::vector<std::string> &taken) {
    const auto used = [&](const std::string &name) {
      return std::ranges::find(taken, name) != taken.end() ||
             std::ranges::find(own.nodes, name, &VP::Node::name) != own.nodes.end();
    };
    std::string name = wanted;
    for (int count = 2; used(name); ++count)
      name = std::format("{}{}", wanted, count);
    return name;
  }

  // Each view's window to its pane while its deck shows it, and the presented one's to
  // the ports its to param names too: a line only for a connection that differs from the
  // one there, those going first, as a window joins one connection at a time.
  void connect(VP::CommandPort &commands, const VP::View &own) {
    struct Wanted {
      std::string name;
      std::string from;
      std::string to;
    };
    const auto hosted = [&](std::string_view view) { return hosts(view); };
    std::vector<Wanted> lines;
    bool presenting_made = false;
    for (const Pane &pane : panes_in(own)) {
      const auto deck = std::ranges::find(own.nodes, pane.deck, &VP::Node::name);
      const bool presented = !pane.view.empty() && pane.view == _selected;
      const bool shown = deck != own.nodes.end() && hosts(pane.view) &&
                         deck_shows(*deck, _selected, hosted) == pane.view;
      if (!presented && !shown)
        continue;
      std::string to = presented ? _to : std::string();
      for (const std::string &picture : _pictures)
        to += std::format(" {}.{}", pane.node, picture);
      presenting_made = presenting_made || presented;
      std::string name = presented
                             ? std::string(presenting)
                             : std::format("{}{}", window_prefix, short_name(pane.node));
      lines.push_back({std::move(name), pane.view, std::move(to)});
    }
    if (!presenting_made && hosts(_selected) && !_to.empty())
      lines.push_back({std::string(presenting), _selected, _to});
    std::vector<std::string> going;
    for (const VP::Connection &connection : own.connections) {
      if (connection.name != presenting && !connection.name.starts_with(window_prefix))
        continue;
      std::string to;
      for (const VP::Endpoint &end : connection.to)
        to += " " + end.text();
      const auto kept = std::ranges::find_if(lines, [&](const Wanted &line) {
        return line.name == connection.name && connection.from.view() &&
               line.from == connection.from.node && words(line.to) == words(to);
      });
      if (kept == lines.end())
        going.push_back(connection.name);
      else
        lines.erase(kept);
    }
    for (const std::string &name : going)
      commands.send(std::format("{}: disconnect {}", _view, name));
    for (const Wanted &line : lines)
      commands.send(
          std::format("{}: connect {} {}: {}", _view, line.name, line.from, line.to));
  }

  // Ports joined by blanks, as the same ports whatever blanks lead them.
  static std::string_view words(std::string_view text) {
    const std::size_t start = text.find_first_not_of(' ');
    return start == std::string_view::npos ? std::string_view() : text.substr(start);
  }

  std::string _name;
  std::string _view;   // the one it runs in, which its lines change
  std::string _parent; // the node it is inside, which its panes are inside too
  std::string _to;     // the ports the window presented reaches too, blank between
  std::string _pane;   // the recipe of a pane
  std::vector<std::string> _rooms;    // the ports inside a pane its room reaches
  std::vector<std::string> _keys;     // and the keys part's typed
  std::vector<std::string> _pictures; // and its view's window
  std::vector<std::string> _hosted;   // the views hosted, as the frame before found them
  std::vector<std::string> _closed;   // those closed since the frame before
  std::string _selected;              // the view presented, or about to be
  std::string _previous;              // the one before it
  bool _started = false; // after the first frame, which may present the one hosted last
  bool _dirty = true;    // the panes, their rooms and the windows follow what is
};

} // namespace

VP_OPERATOR(Panes);
