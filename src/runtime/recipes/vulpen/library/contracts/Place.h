#pragma once

#include <string>

namespace VP_VIEW {

// "And if I go and prepare a place for you, I will come again, and receive you unto
// myself; that where I am, there ye may be also." John 14:3

// Where a graph is, as a terminal shows it in its prompt, as a shell shows the folder it
// is in: the folder of what it shows, absolute: a folder shown in place of a view, or the
// folder of the node whose inside shows (RV08), or of the view shown; empty for none.
struct Place {
  std::string folder;
};

} // namespace VP_VIEW
