#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view clear_screen = "\x1b[2J\x1b[H"; // ANSI: erase it, then home
constexpr std::string_view help_gap = "  "; // between a usage and what it does
constexpr std::string_view prompt_end = "> ";
// What complete reads as a word not begun, since a line's words cannot be empty.
constexpr std::string_view no_word = "\"\"";

// In a window: what it keeps, and how it lays out, in pixels.
constexpr std::size_t scrollback_rows = 1000; // the oldest go first (A03)
constexpr std::size_t history_lines = 100;
constexpr char first_printable = ' '; // the font draws printable ASCII
constexpr char last_printable = '~';
constexpr std::int32_t margin = 4; // around the text
constexpr std::uint32_t rule_width = 1;
constexpr std::uint32_t caret_width = 2;
constexpr std::uint32_t rect_room = 3; // the ground, the rule above the line, the caret
constexpr std::uint32_t label_room = 256;
constexpr std::uint32_t character_room = 65536;

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

// A placeholder, or one in brackets, which a line may leave out.
bool placeholder(std::string_view word) {
  return word.starts_with('<') || word.starts_with('[');
}

// Whether a line names the view it addresses, as `<name>: …` or `: …`.
bool addressed(std::string_view line) {
  const std::vector<std::string_view> words = words_of(line);
  return words.empty() || words.front().ends_with(':');
}

// Each line of a text, without its line breaks, as an answer or the log writes them.
std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> lines;
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    lines.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  return lines;
}

// The last line child list answers: the view hosted most recently, as `<name> <file>`.
std::string_view newest(std::string_view children) {
  std::string_view newest;
  for (const std::string_view line : lines_of(children))
    if (!line.empty())
      newest = line;
  return newest;
}

// What a placeholder stands for in the view: its nodes or connections, or the params of
// the node named just before a key. Any other answers as itself.
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

// The longest start the words share.
std::string_view shared_start(std::span<const std::string_view> words) {
  std::string_view shared = words.front();
  for (const std::string_view word : words.subspan(1)) {
    const auto differs = std::ranges::mismatch(shared, word).in1;
    shared = shared.substr(0, static_cast<std::size_t>(differs - shared.begin()));
  }
  return shared;
}

// What a press on a completion sends: the rest of its word, typed as keys would type
// it; nothing for a placeholder, which is never typed in, or a word typed whole.
std::string rest_typed(std::string_view candidate, std::size_t typed) {
  if (placeholder(candidate) || candidate.size() <= typed)
    return {};
  return std::format("input text {}", candidate.substr(typed));
}

// A log line's color: problems in their own, so they stand out as on the console.
std::uint32_t role_of(VP::Level level) {
  if (level == VP::Level::error)
    return VP_VIEW::role("error");
  if (level == VP::Level::warn)
    return VP_VIEW::role("warning");
  return VP_VIEW::role("text");
}

// The labels and characters a frame writes, filled in turn; what the room lacks is cut.
struct Text {
  std::span<VP_VIEW::Label> labels;
  std::span<VP_VIEW::Character> characters;
  std::uint32_t label_count = 0;
  std::uint32_t character_count = 0;

  void
  add(glm::ivec2 offset, glm::uvec2 extent, std::uint32_t role, std::string_view text) {
    if (label_count == labels.size())
      return;
    const auto shown = static_cast<std::uint32_t>(
        std::min(text.size(), characters.size() - character_count));
    for (std::uint32_t at = 0; at < shown; ++at)
      characters[character_count + at] = {.code = static_cast<unsigned char>(text[at]),
                                          .label = label_count};
    labels[label_count++] = {.offset = offset,
                             .extent = extent,
                             .role = role,
                             .first = character_count,
                             .count = shown};
    character_count += shown;
  }
};

// Where a terminal's text goes in its area, in pixels: the columns of text, where text
// starts, the top of the line typed, the rule above it, and the scrollback's room above
// the rule. It has room for the prompt, a column to type in and a row of scrollback, or
// it shows nothing, as without a font.
struct Grid {
  std::int32_t columns = 0;
  std::int32_t left = 0;
  std::int32_t line = 0;
  std::int32_t rule = 0;
  std::int32_t above = 0;
  bool room = false;
};

// A row of the scrollback, and the role it shows in.
struct Shown {
  std::string text;
  std::uint32_t role = 0;
};

// One line of input with history and completion, sent to the command port. The CLI, the
// terminal and the find bar are this part, so each reaches Vulpen only through the
// command port; its param on says where it is. On the terminal it is the CLI: each line
// typed or piped in runs, what it answers is printed, and the run ends with the input.
// In a window it takes keys from the keys part while it has the focus, which a press in
// its area gives it, and shows what lines answer and the log above the line typed, in
// the Rect its area gives, with a tab for the panel it sits in, named by its title param.
// A line that names no view goes to the view hosted most recently, as view new and view
// load leave it, so it needs no name and every log line still has one. The prompt shows
// that view's folder, as a shell shows the one it is in.
class CommandLine final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _help = node.command("help", "lists every command, with its usage and what it does");
    _complete = node.command(
        "complete <value>...",
        "lists the words that may come next, the last word given being the start of one, "
        "or \"\" for one not begun");
    _clear = node.command("clear", "clears the terminal");
    const std::string on = node.param<std::string>("on");
    if (!on.empty() && on != "terminal" && on != "window") // the loader names it unset
      throw std::runtime_error(std::format("param on = {}: terminal or window", on));
    _window = on == "window";
    if (!_window)
      return;
    _area = &node.input<VP_VIEW::Rect>("area");
    _font = &node.input<VP_VIEW::Font>("font");
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _name = node.name();
    _items = &node.output<VP_VIEW::Items>("items");
    _place = &node.output<VP_VIEW::Rect>("place");
    _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
    _labels = node.upload<VP_VIEW::Label>("labels", label_room);
    _characters = node.upload<VP_VIEW::Character>("characters", character_room);
    _title = node.param<std::string>("title");
    _tabs = &node.output<VP_VIEW::Items>("tabs");
    name_tab();
  }

  void cook(VP::Cook &frame) override {
    if (_window)
      in_window(frame);
    else
      on_terminal(frame);
  }

  void on_terminal(VP::Cook &frame) {
    VP::TerminalPort &terminal = frame.terminal();
    const std::span<const std::string> lines = terminal.lines();
    for (const std::string &line : lines)
      if (const std::string answer = run(frame, line); !answer.empty())
        terminal.print(answer);
    if (terminal.ended()) {
      terminal.prompt(
          "\n"); // ends the prompt's line, as a shell does at the end of input
      frame.commands().send("quit");
    } else if (!lines.empty() || !_prompted) {
      terminal.prompt(prompt());
      _prompted = true;
    }
  }

  // The log since the frame before and the keys pressed, then all of it drawn anew. With
  // no room it takes no keys, so a line never runs where nobody sees it typed.
  void in_window(VP::Cook &frame) {
    for (const VP::Logged &line : frame.commands().log())
      show(line.text, role_of(line.level));
    take_focus(frame);
    if (grid().room)
      for (const VP::Event &event : _typed->to(_name))
        edit(frame, event);
    lay_out(frame);
  }

  // A press in its area gives it the focus, so typing goes where the pointer was; in the
  // order the events came, so a press is tested where the pointer was then.
  void take_focus(VP::Cook &frame) {
    for (const VP::Event &event : frame.input().events())
      if (event.kind == VP::Event::Kind::pointer)
        _pointer = event.at;
      else if (event.kind == VP::Event::Kind::button && event.down &&
               event.name == "left" && VP_VIEW::contains(*_area, _pointer) && !focused())
        frame.commands().send(std::format("focus {}", _name));
  }

  bool focused() const {
    return _typed->focus == _name;
  }

  // Sends a line to the view it names, else to the one hosted most recently, and
  // returns what its command answers.
  std::string run(VP::Cook &frame, std::string_view line) {
    std::string answer = frame.commands().send(address(line));
    current(newest(frame.commands().send(": child list")));
    return answer;
  }

  std::string address(std::string_view line) const {
    return _view.empty() || addressed(line) ? std::string(line)
                                            : std::format("{}: {}", _view, line);
  }

  std::string prompt() const {
    return _folder + std::string(prompt_end);
  }

  // As the area, the font and the prompt are now.
  Grid grid() const {
    const glm::ivec2 size(_area->extent);
    const glm::ivec2 cell(_font->cell);
    Grid grid{.columns = cell.x == 0 ? 0 : (size.x - 2 * margin) / cell.x,
              .left = _area->offset.x + margin,
              .line = _area->offset.y + size.y - margin - cell.y};
    grid.rule = grid.line - margin;
    grid.above = grid.rule - _area->offset.y - 2 * margin;
    grid.room =
        grid.columns > static_cast<std::int32_t>(_folder.size() + prompt_end.size()) &&
        grid.above >= cell.y;
    return grid;
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
    name_tab();
  }

  // Its title, and the view a line goes to unless that is the host, as a shell's title
  // names the folder it is in.
  void name_tab() {
    if (!_window)
      return;
    const std::string label =
        _view.empty() ? _title : std::format("{} - {}", _title, _view);
    _tabs->assign(1, {.label = label, .current = true});
  }

  // Typed text goes in at the caret, and a key edits the line, sends it, recalls one
  // sent before or completes the word at the caret; any other closes the completions.
  void edit(VP::Cook &frame, const VP::Event &event) {
    if (event.kind == VP::Event::Kind::text) {
      for (const char typed : event.name)
        if (typed >= first_printable && typed <= last_printable)
          _line.insert(_caret++, 1, typed);
      _items->clear();
      return;
    }
    if (event.kind != VP::Event::Kind::key || !event.down)
      return;
    const std::string_view key = event.name;
    if (key == "tab")
      return complete_word(frame);
    _items->clear();
    if (key == "enter")
      submit(frame);
    else if (key == "backspace" && _caret > 0)
      _line.erase(--_caret, 1);
    else if (key == "delete" && _caret < _line.size())
      _line.erase(_caret, 1);
    else if (key == "left" && _caret > 0)
      --_caret;
    else if (key == "right" && _caret < _line.size())
      ++_caret;
    else if (key == "home" || key == "end")
      _caret = key == "home" ? 0 : _line.size();
    else if (key == "up" || key == "down")
      recall(key == "up");
  }

  // A blank line only shows the prompt again, as a shell's does.
  void submit(VP::Cook &frame) {
    const std::string line = std::exchange(_line, {});
    _caret = 0;
    show(prompt() + line, VP_VIEW::role("text"));
    if (line.find_first_not_of(' ') == std::string::npos)
      return;
    if (_history.empty() || _history.back() != line)
      _history.push_back(line);
    if (_history.size() > history_lines)
      _history.erase(_history.begin());
    _recalled = _history.size();
    show(run(frame, line), VP_VIEW::role("text"));
  }

  // Up steps back through the lines sent, down forward, past the newest to a new line.
  void recall(bool back) {
    if (back ? _recalled == 0 : _recalled >= _history.size())
      return;
    _recalled = back ? _recalled - 1 : _recalled + 1;
    _line = _recalled < _history.size() ? _history[_recalled] : std::string();
    _caret = _line.size();
  }

  // As a shell does: the word at the caret takes what every candidate shares, a word
  // only one candidate fits is finished, and several show in the list until the next
  // key, where a press on one types the rest of it. A placeholder, as <name>, shows what
  // may come but is never typed in.
  void complete_word(VP::Cook &frame) {
    const std::string_view before = std::string_view(_line).substr(0, _caret);
    _word = before.find_last_of(' ') + 1; // 0 when there is none
    const std::string_view word = before.substr(_word);
    const std::string answer = frame.commands().send(address(std::format(
        "complete {}{}", before.substr(0, _word), word.empty() ? no_word : word)));
    const std::vector<std::string_view> candidates = lines_of(answer);
    std::vector<std::string_view> typeable;
    std::ranges::copy_if(
        candidates, std::back_inserter(typeable), [](std::string_view candidate) {
          return !placeholder(candidate);
        });
    const std::string_view shared = typeable.empty() ? word : shared_start(typeable);
    const std::size_t typed = shared.size(); // the word at the caret, as completed
    _line.insert(_caret, shared.substr(word.size()));
    _caret += typed - word.size();
    if (candidates.size() == 1 && typeable.size() == 1) {
      _line.insert(_caret++, 1, ' ');
      return _items->clear();
    }
    _items->clear();
    for (const std::string_view candidate : candidates)
      _items->push_back(
          {.label = std::string(candidate), .command = rest_typed(candidate, typed)});
  }

  // Each line of the text a row, the oldest dropped past the scrollback's room.
  void show(std::string_view text, std::uint32_t role) {
    for (const std::string_view line : lines_of(text))
      _scrollback.push_back({std::string(line), role});
    while (_scrollback.size() > scrollback_rows)
      _scrollback.pop_front();
  }

  // The scrollback fills the area down to the line typed, its newest row last, as a
  // shell's does. A row wider than the area is cut, and the line typed scrolls to keep
  // the caret in view.
  void lay_out(VP::Cook &frame) {
    const VP_VIEW::Rect &area = *_area;
    const glm::ivec2 cell(_font->cell);
    const auto [columns, left, line, rule, above, room] = grid();
    *_place = {};
    if (!room) {
      frame.write(_rects, 0);
      frame.write(_labels, 0);
      frame.write(_characters, 0);
      return;
    }
    const std::string start = prompt();
    const auto prompt_columns = static_cast<std::int32_t>(start.size());
    // The caret shows only in focus, so it marks where typing goes.
    const std::span<VP_VIEW::Rect> rects =
        frame.write(_rects, focused() ? rect_room : rect_room - 1);
    rects[0] = {.offset = area.offset,
                .extent = area.extent,
                .role = VP_VIEW::role("background")};
    rects[1] = {.offset = {area.offset.x, rule},
                .extent = {area.extent.x, rule_width},
                .role = VP_VIEW::role("border")};
    Text text{frame.write(_labels), frame.write(_characters)};
    const auto rows = static_cast<std::size_t>(above / cell.y);
    const std::size_t shown = std::min(rows, _scrollback.size());
    const glm::uvec2 row_extent(static_cast<std::uint32_t>(columns * cell.x), cell.y);
    for (std::size_t row = 0; row < shown; ++row) {
      const Shown &kept = _scrollback[_scrollback.size() - shown + row];
      const auto from_rule = static_cast<std::int32_t>(shown - row) * cell.y;
      text.add({left, rule - margin - from_rule},
               row_extent,
               kept.role,
               std::string_view(kept.text).substr(0, static_cast<std::size_t>(columns)));
    }
    const VP_VIEW::Rect caret =
        lay_out_line(text, start, left, columns - prompt_columns, cell, line);
    if (focused())
      rects[2] = caret;
    frame.write(_labels, text.label_count);
    frame.write(_characters, text.character_count);
    place_list(left, columns - prompt_columns, cell, rule, rows);
  }

  // The prompt and the line typed, scrolled so the caret stays in view; returns the
  // caret.
  VP_VIEW::Rect lay_out_line(Text &text,
                             std::string_view start,
                             std::int32_t left,
                             std::int32_t room,
                             glm::ivec2 cell,
                             std::int32_t line) {
    const auto columns = static_cast<std::size_t>(room);
    _first = _caret < columns ? 0 : _caret - columns + 1;
    const std::int32_t typed = left + static_cast<std::int32_t>(start.size()) * cell.x;
    text.add({left, line},
             glm::uvec2(static_cast<std::uint32_t>(typed - left), cell.y),
             VP_VIEW::role("accent"),
             start);
    text.add({typed, line},
             glm::uvec2(static_cast<std::uint32_t>(room * cell.x), cell.y),
             VP_VIEW::role("text"),
             std::string_view(_line).substr(_first, columns));
    const auto caret = static_cast<std::int32_t>(_caret - _first);
    return {.offset = {typed + caret * cell.x, line},
            .extent = {caret_width, static_cast<std::uint32_t>(cell.y)},
            .role = VP_VIEW::role("accent")};
  }

  // The completions, as many as fit, just above the rule, from the word they complete.
  void place_list(std::int32_t left,
                  std::int32_t room,
                  glm::ivec2 cell,
                  std::int32_t rule,
                  std::size_t rows) {
    if (_items->empty())
      return;
    std::size_t longest = 0;
    for (const VP_VIEW::Item &item : *_items)
      longest = std::max(longest, item.label.size());
    const auto count = static_cast<std::int32_t>(std::min(_items->size(), rows));
    const auto wide = static_cast<std::int32_t>(
                          std::min(longest, static_cast<std::size_t>(room))) *
                      cell.x;
    const std::int32_t word = static_cast<std::int32_t>(_word) -
                              static_cast<std::int32_t>(_first);
    const std::int32_t typed = left + static_cast<std::int32_t>(prompt().size()) * cell.x;
    const std::int32_t x =
        std::clamp(typed + word * cell.x, typed, typed + (room * cell.x) - wide);
    *_place = {.offset = {x, rule - margin - count * cell.y},
               .extent = glm::uvec2(static_cast<std::uint32_t>(wide),
                                    static_cast<std::uint32_t>(count * cell.y))};
  }

  void command(VP::Call &call) override {
    if (call.is(_help))
      help(call);
    else if (call.is(_complete))
      complete(call);
    else if (call.is(_clear) && _window)
      _scrollback.clear();
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
    const std::string_view begun = typed.back() == no_word ? "" : typed.back();
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
        if (word.starts_with(begun) || placeholder(word))
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
  bool _window = false;
  // In a window: what it reads and writes, and the line being typed.
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  std::string _name;                // as the focus names it
  glm::vec2 _pointer{};             // as the events so far left it
  VP_VIEW::Items *_items = nullptr; // the completions shown, empty for none
  VP_VIEW::Rect *_place = nullptr;  // where they show
  std::string _title;
  VP_VIEW::Items *_tabs = nullptr; // the one a panel shows for it
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels;
  VP::Upload<VP_VIEW::Character> _characters;
  std::deque<Shown> _scrollback;
  std::string _line;
  std::size_t _caret = 0;
  std::size_t _first = 0; // the first character of the line shown
  std::size_t _word = 0;  // where the word the completions complete starts
  std::vector<std::string> _history;
  std::size_t _recalled = 0; // the line recalled; past the history for a new one
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<CommandLine>("CommandLine");
}
