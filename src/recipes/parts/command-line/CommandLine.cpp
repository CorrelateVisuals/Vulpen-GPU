#include "runtime/Operator.h"

#include <algorithm>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view clear_screen = "\x1b[2J\x1b[H"; // ANSI: erase it, then home
constexpr std::string_view help_gap = "  "; // between a usage and what it does

// A usage's words, split at its blanks as the command port splits a line.
std::vector<std::string_view> words_of(std::string_view text) {
  std::vector<std::string_view> words;
  for (std::size_t at = text.find_first_not_of(' '); at != std::string_view::npos;) {
    const std::size_t end = std::min(text.find(' ', at), text.size());
    words.push_back(text.substr(at, end - at));
    at = text.find_first_not_of(' ', end);
  }
  return words;
}

bool placeholder(std::string_view word) {
  return word.starts_with('<');
}

// One line of input with history and completion, sent to the command port. The CLI, the
// terminal and the find bar are this part, so each reaches Vulpen only through the
// command port. On the terminal it is the CLI: each line typed or piped in runs, what it
// answers is printed, and the run ends with the input.
class CommandLine final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _help = node.command("help", "lists every command, with its usage and what it does");
    _complete = node.command(
        "complete <value>...",
        "lists the words that may come next, the last word given being the start of one");
    _clear = node.command("clear", "clears the terminal");
  }

  void cook(VP::Cook &frame) override {
    VP::TerminalPort &terminal = frame.terminal();
    for (const std::string &line : terminal.lines())
      if (const std::string answer = frame.commands().send(line); !answer.empty())
        terminal.print(answer);
    if (terminal.ended())
      frame.commands().send("quit");
  }

  void command(VP::Call &call) override {
    if (call.is(_help))
      help(call);
    else if (call.is(_complete))
      complete(call);
    else if (call.is(_clear))
      call.reply(clear_screen);
  }

  // Usages aligned, so what each does reads as a column.
  static void help(VP::Call &call) {
    const std::vector<VP::Usage> usages = call.commands().usages();
    std::size_t width = 0;
    for (const VP::Usage &usage : usages)
      width = std::max(width, usage.usage.size());
    for (const VP::Usage &usage : usages)
      call.reply(std::string(usage.usage)
                     .append(width - usage.usage.size(), ' ')
                     .append(help_gap)
                     .append(usage.help));
  }

  // A placeholder answers as itself, such as <node>, until the part reads the graph.
  static void complete(VP::Call &call) {
    const std::span<const std::string_view> typed = call.arguments();
    std::set<std::string_view> found; // sorted, and each once
    for (const VP::Usage &usage : call.commands().usages()) {
      const std::vector<std::string_view> words = words_of(usage.usage);
      if (words.size() < typed.size())
        continue;
      const bool fits =
          std::ranges::equal(typed.first(typed.size() - 1),
                             std::span(words).first(typed.size() - 1),
                             [](std::string_view given, std::string_view word) {
                               return placeholder(word) || given == word;
                             });
      const std::string_view next = words[typed.size() - 1];
      if (fits && (placeholder(next) || next.starts_with(typed.back())))
        found.insert(next);
    }
    for (const std::string_view word : found)
      call.reply(word);
  }

  VP::Command _help;
  VP::Command _complete;
  VP::Command _clear;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<CommandLine>("CommandLine");
}
