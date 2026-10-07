#include "contracts/Rect.h"
#include "runtime/Operator.h"

namespace {

// Hands on the whole Rect a frame draws into, the window's, as Vulkan's viewport names
// it, so the outermost split of a dock divides it as any other Rect, and a dock nests
// in a dock unchanged. Empty without a window.
class Viewport final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _area = &node.output<VP_VIEW::Rect>("area");
  }
  void cook(VP::Cook &frame) override {
    *_area = {.extent = frame.resolution()};
  }

  VP_VIEW::Rect *_area = nullptr;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Viewport>("Viewport");
}
