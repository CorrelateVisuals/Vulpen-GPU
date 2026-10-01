#include "runtime/Views.h"

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

namespace VP {

// Hosts no view yet.
View *Views::find(std::string_view) {
  return nullptr;
}

void Views::command(Call &) {}

} // namespace VP
