#include "baseclasses/Engine.h"
#include "baseclasses/Resources.h"
#include "runtime/Operator.h"

#include <optional>
#include <stdexcept>

namespace {

constexpr VkDeviceSize scratch_bytes = 4096;

// Makes a buffer through the engine by hand, as a module that calls the executable's
// code: it owns the buffer, and destroys it before its module goes.
class Scratch final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _buffer.emplace(node.engine().resources().buffer(
        scratch_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VP::Memory::upload));
    if (_buffer->bytes().size() != scratch_bytes)
      throw std::runtime_error("the engine mapped a buffer of another size");
  }

  std::optional<VP::Buffer> _buffer;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Scratch>("Scratch");
}
