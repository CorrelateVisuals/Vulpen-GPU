#pragma once

namespace VP {

// Every view this process runs: the one it started with and the views it hosts (V03),
// each with its schedule. Hosting is session state, so the log keeps it and the host's
// manifest does not; a command reaches a hosted view by its name.
class Views {};

} // namespace VP
