#include "contracts/Item.h"
#include "runtime/Operator.h"

#include <optional>
#include <utility>

namespace {

// Joins two lists of Items into one, the first's then the second's, a divider between
// them, as a bar shows the buttons two parts offer in one row; written anew only as
// either changes, so a frame allocates nothing for them (CPP10).
class Join final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _first = &node.input<VP_VIEW::Items>("first");
    _second = &node.input<VP_VIEW::Items>("second");
    _items = &node.output<VP_VIEW::Items>("items");
    _joined.reset();
  }

  void cook(VP::Cook &) override {
    if (_joined && _joined->first == *_first && _joined->second == *_second)
      return;
    _joined.emplace(*_first, *_second);
    *_items = *_first;
    if (!_first->empty() && !_second->empty())
      _items->emplace_back(); // a divider, an Item with no label
    _items->insert(_items->end(), _second->begin(), _second->end());
  }

  const VP_VIEW::Items *_first = nullptr;
  const VP_VIEW::Items *_second = nullptr;
  VP_VIEW::Items *_items = nullptr;
  // What the Items were joined from, so the next frame joins again only on a change;
  // none until the first frame, and after a rebuild.
  std::optional<std::pair<VP_VIEW::Items, VP_VIEW::Items>> _joined;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Join>("Join");
}
