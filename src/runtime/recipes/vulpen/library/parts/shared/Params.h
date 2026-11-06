#pragma once

#include <algorithm>
#include <format>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// A param that takes one of a closed set of words, as an axis takes x or y: any other
// word is refused naming them all. One left unset reads empty, and the binder names it
// already, so it is named once (A02).
void check_word(std::string_view key,
                std::string_view value,
                std::initializer_list<std::string_view> words) {
  if (value.empty() || std::ranges::find(words, value) != words.end())
    return;
  std::string named;
  for (const std::string_view word : words)
    named.append(named.empty() ? "" : word == *(words.end() - 1) ? " or " : ", ")
        .append(word);
  throw std::runtime_error(std::format("param {} = {}: {}", key, value, named));
}

} // namespace
