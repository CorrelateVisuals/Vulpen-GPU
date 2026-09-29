#pragma once

#include "runtime/Operator.h"

// Offers the commands that apply to a target, each as an Item carrying its text form, so
// a menu can do nothing the CLI cannot.
class CommandItems : public VP::Operator {};
