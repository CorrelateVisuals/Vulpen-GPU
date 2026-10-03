#include "runtime/Operator.h"

namespace {

// Registers the command fill's Twin registers, so of two nodes that run them, the
// second must be refused.
class Twin final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.command("fill twin", "registers in every node that runs it");
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Twin>("Twin");
}
