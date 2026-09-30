#include "runtime/Operator.h"
#include "runtime/View.h"

namespace {

// Reads the graph and answers ls and info, so a view can be read without a window and a
// front end needs no mirror of the graph of its own.
class Inspect final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<Inspect>("Inspect");
}
