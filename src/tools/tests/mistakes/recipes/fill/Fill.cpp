#include "runtime/Operator.h"

#include <cstdint>

namespace {

// Each sets a value Fill.comp does not declare so: the loader must refuse the node.
class WrongName final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.value<float>("level");
  }
};

class WrongType final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.value<std::uint32_t>("amount");
  }
};

} // namespace

VP_RECIPE(registry) {
  registry.add<WrongName>("WrongName");
  registry.add<WrongType>("WrongType");
}
