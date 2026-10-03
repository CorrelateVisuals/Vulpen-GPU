#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view clear_screen = "\x1b[2J\x1b[H"; // ANSI: erase it, then home
constexpr std::string_view help_gap = "  "; // between a usage and what it does
constexpr std::string_view prompt_end = "> ";

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

// Whether a line names the view it addresses, as `<name>: …` or `: …`.
bool addressed(std::string_view line) {
  const std::vector<std::string_view> words = words_of(line);
  return words.empty() || words.front().ends_with(':');
}

// The last line child list answers: the view hosted most recently, as `<name> <file>`.
std::string_view newest(std::string_view children) {
  std::string_view newest;
  for (std::size_t at = 0; at < children.size();) {
    const std::size_t end = std::min(children.find('\n', at), children.size());
    if (const std::string_view line = children.substr(at, end - at); !line.empty())
      newest = line;
    at = end + 1;
  }
  return newest;
}

// What a placeholder stands for in the view: its nodes, connections or deploys, or the
// params of the node named just before a key. Any other answers as itself.
std::vector<std::string> names(std::string_view kind,
                               const VP::View &view,
                               std::span<const std::string_view> typed) {
  std::vector<std::string> names;
  if (kind == "<node>") {
    for (const VP::Node &node : view.nodes)
      names.push_back(node.name);
  } else if (kind == "<connection>") {
    for (const VP::Connection &connection : view.connections)
      names.push_back(connection.name);
  } else if (kind == "<deploy>") {
    for (const VP::Deploy &deploy : view.deploys)
      names.push_back(deploy.name);
  } else if (kind == "<key>" && typed.size() > 1) {
    const auto node =
        std::ranges::find(view.nodes, typed[typed.size() - 2], &VP::Node::name);
    if (node != view.nodes.end())
      for (const VP::Param &param : node->params)
        names.push_back(param.key);
  } else {
    names.emplace_back(kind);
  }
  return names;
}

// One line of input with history and completion, sent to the command port. The CLI, the
// terminal and the find bar are this part, so each reaches Vulpen only through the
// command port. On the terminal it is the CLI: each line typed or piped in runs, what it
// answers is printed, and the run ends with the input. A line that names no view goes to
// the view hosted most recently, as view new and view load leave it, so it needs no name
// and every log line still has one. The prompt shows that view's folder, as a shell
// shows the one it is in.
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
    const std::span<const std::string> lines = terminal.lines();
    for (const std::string &line : lines) {
      const std::string sent =
          _view.empty() || addressed(line) ? line : _view + ": " + line;
      if (const std::string answer = frame.commands().send(sent); !answer.empty())
        terminal.print(answer);
      current(newest(frame.commands().send(": child list")));
    }
    if (terminal.ended()) {
      terminal.prompt(
          "\n"); // ends the prompt's line, as a shell does at the end of input
      frame.commands().send("quit");
    } else if (!lines.empty() || !_prompted) {
      terminal.prompt(_folder + std::string(prompt_end));
      _prompted = true;
    }
  }

  // From where vulpen started, as a shell names a folder; the host has no name to show.
  void current(std::string_view newest) {
    const std::size_t space = newest.find(' ');
    _view = newest.substr(0, space);
    _folder = space == std::string_view::npos
                  ? std::string()
                  : std::filesystem::path(newest.substr(space + 1))
                        .parent_path()
                        .lexically_proximate(std::filesystem::current_path())
                        .generic_string();
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

  static void complete(VP::Call &call) {
    const std::span<const std::string_view> typed = call.arguments();
    std::set<std::string> found; // sorted, and each once
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
      if (!fits)
        continue;
      for (const std::string &word : placeholder(next) ? names(next, call.view(), typed)
                                                       : std::vector{std::string(next)})
        if (word.starts_with(typed.back()) || placeholder(word))
          found.insert(word);
    }
    for (const std::string &word : found)
      call.reply(word);
  }

  VP::Command _help;
  VP::Command _complete;
  VP::Command _clear;
  std::string _view;   // the view a line that names none goes to; empty for the host
  std::string _folder; // that view's, as the prompt shows it
  bool _prompted = false;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<CommandLine>("CommandLine");
}
