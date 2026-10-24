#include "contracts/Item.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::array<std::string_view, 2> halves{"first", "second"};

// The tabs of two contents a switch holds, as one row: the shown one's as they are, the
// other's unlit, and a press on the one current in it, or on one with no command of its
// own, shows its content, a param set of the switch, which it finds as the node that
// hands it its first; the others keep their own, as a project's tab presents it, which
// then shows itself. Written anew only as they change, so a frame allocates nothing for
// them (CPP10). A separate node from the switch, as the contents read their room from it
// and hand their tabs here: one node for both would close a loop.
class SwitchTabs final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _tabs_of = {&node.input<VP_VIEW::Items>("first-tabs"),
                &node.input<VP_VIEW::Items>("second-tabs")};
    _first = &node.input<VP_VIEW::Rect>("first");
    _tabs = &node.output<VP_VIEW::Items>("tabs");
    _name = node.name();
    _view = node.view_name();
    _written.reset();
  }

  void cook(VP::Cook &frame) override {
    const std::size_t shown = shown_half(frame.view());
    if (_written && _written->shown == shown && _written->tabs[0] == *_tabs_of[0] &&
        _written->tabs[1] == *_tabs_of[1])
      return;
    _written.emplace(Written{shown, {*_tabs_of[0], *_tabs_of[1]}});
    _tabs->clear();
    for (std::size_t half = 0; half < halves.size(); ++half)
      for (VP_VIEW::Item tab : *_tabs_of[half]) {
        if (half != shown && (tab.current || tab.command.empty()))
          tab.command =
              std::format("{}: param set {} shown {}", _view, _switch, halves[half]);
        if (half != shown) {
          tab.current = false;
          tab.role.reset();
        }
        _tabs->push_back(std::move(tab));
      }
  }

  // As the switch's param says, the switch being the node its first comes from.
  std::size_t shown_half(const VP::View &view) {
    for (const VP::Connection &connection : view.connections)
      if (std::ranges::any_of(connection.to, [&](const VP::Endpoint &to) {
            return to.node == _name && to.port == halves[0];
          })) {
        _switch = connection.from.node;
        const auto node = std::ranges::find(view.nodes, _switch, &VP::Node::name);
        if (node == view.nodes.end())
          break;
        const auto shown = std::ranges::find(node->params, "shown", &VP::Param::key);
        return shown != node->params.end() && shown->value == halves[1] ? 1 : 0;
      }
    return 0;
  }

  // The tabs written, and which half showed then.
  struct Written {
    std::size_t shown = 0;
    std::array<VP_VIEW::Items, 2> tabs;
  };

  std::array<const VP_VIEW::Items *, 2> _tabs_of{};
  const VP_VIEW::Rect *_first = nullptr; // read for the switch it names, not its size
  VP_VIEW::Items *_tabs = nullptr;
  std::string _name;
  std::string _view;   // the one it runs in, where its lines set the switch's param
  std::string _switch; // the node that hands it its first
  std::optional<Written> _written;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<SwitchTabs>("SwitchTabs");
}
