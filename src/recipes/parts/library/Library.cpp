#include "runtime/Operator.h"

namespace {

// Works on recipe and view folders through the file port: lists and drops recipes,
// starts one from the template, and makes and loads views. A drop copies files and then
// deploys, so the view owns its copy from the first edit on.
class Library final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<Library>("Library");
}
