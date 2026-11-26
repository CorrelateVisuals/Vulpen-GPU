#pragma once

#include <format>
#include <stdexcept>
#include <utility>

// How the engine throws a worded error, in every file that throws one. At the bottom of
// the include map, it includes nothing of ours, so baseclasses and runtime alike reach
// it.
namespace VP {

// "For by thy words thou shalt be justified, and by thy words thou shalt be condemned."
// Matthew 12:37

// A worded error in one call: the words as std::format makes them, checked as it
// compiles, thrown as std::runtime_error (A02).
template <class... Args>
[[noreturn]] void fail(std::format_string<Args...> words, Args &&...args) {
  throw std::runtime_error(std::format(words, std::forward<Args>(args)...));
}

} // namespace VP
