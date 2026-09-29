#pragma once

#include "runtime/Operator.h"

// Finds the relations no compiler sees (a CMake file naming a source, a doc linking a
// file, a text citing an id) and publishes them, so the graph draws them without parsing
// files itself.
class Relations : public VP::Operator {};
