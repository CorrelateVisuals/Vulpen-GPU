#pragma once

#include "runtime/Commands.h"

#include <string_view>

namespace VP {

// Every view this process runs: the one it started with and the views it hosts (V03),
// each with its schedule. Hosting is session state, so the log keeps it and the host's
// manifest does not; a command reaches a hosted view by its name.
class Views final : public ViewLookup, public CommandHandler {
  View *find(std::string_view name) override;
  void command(Call &call) override;
};

} // namespace VP
