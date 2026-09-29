#pragma once

#include "runtime/Operator.h"

// The CLI is a recipe like any GUI: it reaches Vulpen only through the command port.
class CommandLine : public VP::Operator {};
