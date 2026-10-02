#include "runtime/Operator.h"

#include <cstdint>
#include <stdexcept>

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

// A command without its help: the port must refuse it (RV04).
class NoHelp final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.command("fill help", "");
  }
};

// Every node it runs in registers the same command, so a second node must be refused.
class Twin final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.command("fill twin", "registers in every node that runs it");
  }
};

// Its command fails, so a script that runs it must stop, naming why.
class Refuse final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _refuse = node.command("refuse", "fails, as the fixture means it to");
  }
  void command(VP::Call &call) override {
    if (call.is(_refuse))
      throw std::runtime_error("refused, as the fixture means to");
  }

  VP::Command _refuse;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<WrongName>("WrongName");
  registry.add<WrongType>("WrongType");
  registry.add<NoHelp>("NoHelp");
  registry.add<Twin>("Twin");
  registry.add<Refuse>("Refuse");
}
