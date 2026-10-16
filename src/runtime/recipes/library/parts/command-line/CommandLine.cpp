// Longer than a file should be (CPP12): the line typed, its completion, its history and
// the rows above it are one part's job, and a part's class stays in one file's unnamed
// namespace, so two copies of the part in one binary never clash.
#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Labels.h"
#include "shared/Presented.h"
#include "shared/Scroll.h"
#include "shared/Words.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view clear_screen = "\x1b[2J\x1b[H"; // ANSI: erase it, then home
// Between the columns the terminal prints: a usage and what it does, or candidates.
constexpr std::string_view column_gap = "  ";
constexpr std::size_t help_depth = 2; // words a group's head holds at most: input key
constexpr std::string_view prompt_end = "> ";
// What complete reads as a word not begun, since a line's words cannot be empty.
constexpr std::string_view no_word = "\"\"";
// After a usage's last placeholder: it takes one argument or more.
constexpr std::string_view one_or_more = "...";
constexpr std::string_view output_mark = "{out}"; // a node's lines, as the log marks them

// In a window: what it keeps, and how it lays out, in pixels.
constexpr std::size_t scrollback_rows = 1000; // the oldest go first (A03)
constexpr std::size_t history_lines = 100;
constexpr std::int32_t margin = 4; // around the text
constexpr std::uint32_t rule_width = 1;
constexpr std::uint32_t caret_width = 2;
constexpr std::uint32_t label_room = 256;
constexpr std::size_t rows_most = label_room - 2; // the prompt and the line take two
// The ground, the rule above the line, the caret, the scroll bars' thumbs, and the
// selection a row shown.
constexpr std::uint32_t rect_room = 5 + rows_most;
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

// A usage's placeholder: what it stands for, without the brackets a line may leave it
// out by, and whether it takes more, standing for every word after it too, so
// [<node>...] is a <node> that repeats.
struct Kind {
  std::string_view name;
  bool more = false;
};

Kind kind_of(std::string_view word) {
  if (word.starts_with('[') && word.ends_with(']'))
    word = word.substr(1, word.size() - 2);
  const bool more = word.ends_with(one_or_more);
  if (more)
    word.remove_suffix(one_or_more.size());
  return {word, more};
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

// Of the lines view list answers, a view each as `<name> <file>`, the one lines go to:
// the view presented while it is still hosted, else the one hosted most recently; empty
// for none.
std::string_view followed(std::string_view hosted, std::string_view shown) {
  std::string_view newest;
  for (const std::string_view line : lines_of(hosted)) {
    if (!shown.empty() && line.substr(0, line.find(' ')) == shown)
      return line;
    if (!line.empty())
      newest = line;
  }
  return newest;
}

// The folders and files where a path begun points, each as the whole word, from where
// vulpen started, as a typed line's <file> resolves. A name with a blank would split into
// two words, so it is left out, and a hidden one shows once the name begun starts with a
// dot, as in a shell. A folder that cannot be listed, as a file followed by /, is refused
// by the file port, naming why.
std::vector<std::string> paths(VP::FilePort &files, std::string_view begun) {
  const std::string_view folder = begun.substr(0, begun.rfind('/') + 1);
  const bool hidden = begun.substr(folder.size()).starts_with('.');
  std::vector<std::string> paths;
  for (const std::string &name :
       files.list((std::filesystem::current_path() / folder).string()))
    if ((hidden || !name.starts_with('.')) && name.find(' ') == std::string::npos)
      paths.push_back(std::string(folder).append(name));
  return paths;
}

// What a placeholder stands for: the view's nodes or connections, the params of the
// node named just before a key, or the paths a word begun may become. Any other answers
// as itself.
std::vector<std::string> names(std::string_view word,
                               VP::Call &call,
                               std::span<const std::string_view> typed,
                               std::string_view begun) {
  const std::string_view kind = kind_of(word).name;
  std::vector<std::string> names;
  if (kind == "<node>") {
    for (const VP::Node &node : call.view().nodes)
      names.push_back(node.name);
  } else if (kind == "<connection>") {
    for (const VP::Connection &connection : call.view().connections)
      names.push_back(connection.name);
  } else if (kind == "<key>" && typed.size() > 1) {
    const std::vector<VP::Node> &nodes = call.view().nodes;
    const auto node = std::ranges::find(nodes, typed[typed.size() - 2], &VP::Node::name);
    if (node != nodes.end())
      for (const VP::Param &param : node->params)
        names.push_back(param.key);
  } else if (kind == "<file>" || kind == "<view>") {
    names = paths(call.files(), begun);
  } else {
    names.emplace_back(word);
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

// A log line's color: problems in their own, so they stand out as on the console; what
// a node writes as text, as what lines answer; and the engine's own muted, so the lines
// typed, their answers and the nodes' lines stand out from it.
std::uint32_t role_of(const VP::Logged &line) {
  if (line.level == VP::Level::error)
    return VP_VIEW::role("error");
  if (line.level == VP::Level::warn)
    return VP_VIEW::role("warning");
  return line.text.starts_with(output_mark) ? VP_VIEW::role("text")
                                             : VP_VIEW::role("muted");
}

// Where a terminal's text goes in its area, in pixels: the columns of text, where text
// starts, the top of the line typed, the rule above it, the bottom of the scrollback's
// rows, clear of the bar across, and their room. It has room for the prompt, a column to
// type in and a row of scrollback, or it shows nothing, as without a font.
struct Grid {
  std::int32_t columns = 0;
  std::int32_t left = 0;
  std::int32_t line = 0;
  std::int32_t rule = 0;
  std::int32_t bottom = 0;
  std::int32_t above = 0;
  bool room = false;
};

// A row of the scrollback, and the role it shows in.
struct Shown {
  std::string text;
  std::uint32_t role = 0;
};

// A place in the scrollback: its row, counted from the first row it ever kept, so rows
// dropped from its front leave a mark on its text, and a column of that row.
struct Mark {
  std::size_t row = 0;
  std::size_t column = 0;

  auto operator<=>(const Mark &) const = default;
};

// One line of input with history and completion, sent to the command port. The CLI, the
// terminal and the find bar are this part, so each reaches Vulpen only through the
// command port; its param on says where it is. On the terminal it is the CLI: each line
// typed or piped in runs, what it answers is printed, and the run ends with the input.
// In a window it takes keys from the keys part while it has the focus, which a press in
// its area gives it, and shows what lines answer and the log above the line typed, in
// the Rect its area gives, with a tab for the panel it sits in, named by its title param;
// a drag selects in what it shows, as a double click does a word and one more press its
// row, which control with c copies, and the wheel or a drag on the bar at its right edge
// scrolls it; turned sideways or with shift, or a drag on the bar above the rule, scrolls
// its rows across, while the line typed keeps its caret in view. The terminal vulpen was
// started from takes lines then too, as the CLI's does, but its end ends no run.
// A line that names no view goes to the view presented, whose window a connection of its
// view takes, as the frame began, while it is still hosted; else to the one hosted most
// recently, as view new and view load leave it. So it needs no name, every log line still
// has one, and it works on the view the window shows, as the graph does. The prompt shows
// that view's folder, as a shell shows the one it is in, and the log keeps to that view.
class CommandLine final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _help = node.command(
        "help", "lists every command, under the words it shares, and what each does");
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
    _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
    _labels = node.upload<VP_VIEW::Label>("labels", label_room);
    _characters = node.upload<VP_VIEW::Character>("characters", character_room);
    _title = node.param<std::string>("title");
    _tabs = &node.output<VP_VIEW::Items>("tabs");
    name_tab();
  }

  void cook(VP::Cook &frame) override {
    follow(frame);
    keep_log(frame);
    if (_window)
      in_window(frame);
    on_terminal(frame);
  }

  // The log keeps to the view lines go to and the views it hosts, so with views running
  // side by side, the terminal vulpen was started from and the rows here show the one
  // worked on, and every view's problems; told only when they change, so a frame
  // allocates nothing for them.
  void keep_log(VP::Cook &frame) {
    const VP::View *const followed = _view.empty() ? nullptr : frame.hosted(_view);
    const std::span<const VP::Child> inside =
        followed ? std::span(followed->children) : std::span<const VP::Child>();
    const std::size_t count = _view.empty() ? 0 : 1 + inside.size();
    bool same = _kept.size() == count && (count == 0 || _kept.front() == _view);
    for (std::size_t at = 0; same && at < inside.size(); ++at)
      same = _kept[at + 1] == inside[at].name;
    if (same)
      return;
    _kept.clear();
    if (!_view.empty())
      _kept.push_back(_view);
    for (const VP::Child &child : inside)
      _kept.push_back(child.name);
    frame.commands().keep_log_to(_kept);
  }

  // Once the frame shows another view presented, or another hosted most recently, as at
  // the first frame, the view lines go to is found again; a frame between asks nothing.
  void follow(VP::Cook &frame) {
    const VP::View &view = frame.view();
    const std::string_view shown = presented(view);
    const std::string_view newest =
        view.children.empty() ? std::string_view() : view.children.back().name;
    if (_followed && shown == _presented && newest == _newest)
      return;
    _followed = true;
    _presented = shown;
    _newest = newest;
    current(followed(frame.commands().send(": view list"), _presented));
  }

  void on_terminal(VP::Cook &frame) {
    VP::TerminalPort &terminal = frame.terminal();
    const std::span<const std::string> lines = terminal.lines();
    for (const std::string &line : lines)
      if (const std::string answer = run(frame, line); !answer.empty())
        terminal.print(answer);
    if (terminal.ended() && !_window) {
      terminal.prompt(
          "\n"); // ends the prompt's line, as a shell does at the end of input
      frame.commands().send("quit");
    } else if (!lines.empty() || !_prompted) {
      terminal.prompt(prompt());
      _prompted = true;
    }
  }

  // The log since the frame before and the keys pressed, then all of it drawn anew. With
  // no room it takes no keys, so a line never runs where nobody sees it typed. A key let
  // go while another part had the focus never came, so out of focus control counts as up.
  void in_window(VP::Cook &frame) {
    fit(0);
    for (const VP::Logged &line : frame.commands().log())
      show(line.text, role_of(line));
    point(frame);
    if (!focused())
      _control = false;
    if (grid().room)
      for (const VP::Event &event : _typed->to(_name))
        edit(frame, event);
    lay_out(frame);
  }

  // A press in its area gives it the focus, so typing goes where the pointer was, and one
  // on the rows above the line starts a selection there, which a drag stretches; in the
  // order the events came, so a press is tested where the pointer was then. A press on
  // a scroll bar only scrolls. The wheel scrolls across as it turns sideways, or with
  // shift held, which the window says whatever part has the focus.
  void point(VP::Cook &frame) {
    const Grid grid = this->grid();
    const VP_VIEW::Rect rows = rows_area(grid);
    for (const VP::Event &event : frame.input().events()) {
      const bool inside = VP_VIEW::contains(*_area, _pointer);
      if (event.kind == VP::Event::Kind::pointer) {
        _pointer = event.at;
        _scroll.drag(_pointer, _scroll.track(rows));
        _columns.drag(_pointer, _columns.track(rows));
        if (_selecting)
          _end = mark_at(grid);
      } else if (event.kind == VP::Event::Kind::wheel && inside && grid.room) {
        const glm::vec2 turn = _sideways ? glm::vec2(event.turn.y, 0.0f) : event.turn;
        _scroll.wheel(turn.y);
        _columns.wheel(turn.x);
      } else if (event.kind == VP::Event::Kind::key && event.name == "shift") {
        _sideways = event.down;
      } else if (event.kind == VP::Event::Kind::focus && !event.down) {
        _sideways = false; // let go while the window had no focus, which no event says
      } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
        if (!event.down) {
          _scroll.release();
          _columns.release();
        }
        if (event.down && inside && grid.room &&
            (_scroll.press(_pointer, _scroll.track(rows)) ||
             _columns.press(_pointer, _columns.track(rows))))
          continue;
        _selecting = event.down && inside && grid.room && !_scrollback.empty() &&
                     _pointer.y < static_cast<float>(grid.rule);
        if (event.down && inside)
          _anchor = _end = _selecting ? mark_at(grid) : _end;
        if (_selecting && event.presses > 1)
          select_run(grid, event.presses);
        if (event.down && inside && !focused())
          frame.commands().send(std::format("focus {}", _name));
      }
    }
  }

  // The second press in a row selects the word pressed, and any more the row; a drag
  // leaves either as it is.
  void select_run(const Grid &grid, std::uint32_t presses) {
    const Mark pressed = mark_at(grid, true);
    const std::string &row = _scrollback[pressed.row - _dropped].text;
    const auto [from, to] = presses == 2 ? word_at(row, pressed.column)
                                         : std::pair{std::size_t{0}, row.size()};
    _anchor = {pressed.row, from};
    _end = {pressed.row, to};
    _selecting = false;
  }

  // The place nearest the pointer in the rows shown: the row it is on, or the nearest,
  // and the boundary between characters closest to it, as a press puts the editor's
  // caret; or with character, the start of the character it is on.
  Mark mark_at(const Grid &grid, bool character = false) const {
    const glm::vec2 cell(_font->cell);
    const std::size_t shown = rows_shown(grid);
    if (shown == 0) // cleared, or given no room, while dragged
      return _end;
    const glm::vec2 from(static_cast<float>(grid.left),
                         static_cast<float>(grid.bottom) -
                             static_cast<float>(shown) * cell.y);
    const glm::vec2 cells = (_pointer - from) / cell;
    const auto row = static_cast<std::size_t>(
        std::clamp(std::floor(cells.y), 0.0f, static_cast<float>(shown - 1)));
    const std::size_t index = _scroll.first() + row;
    const float across = character ? std::floor(cells.x) : std::round(cells.x);
    const std::size_t column =
        _columns.first() + static_cast<std::size_t>(std::max(0.0f, across));
    return {_dropped + index, std::min(column, _scrollback[index].text.size())};
  }

  // Of the scrollback, as many of its newest rows as fit above the line, and as Labels
  // the line leaves.
  std::size_t rows_shown(const Grid &grid) const {
    const auto rows = grid.above / static_cast<std::int32_t>(_font->cell.y);
    return std::min({static_cast<std::size_t>(rows), _scrollback.size(), rows_most});
  }

  // The selection as text, a break between its rows; what the scrollback dropped since
  // is gone from it.
  std::string selected() const {
    const auto [from, to] = std::minmax(_anchor, _end);
    std::string text;
    for (std::size_t row = std::max(from.row, _dropped);
         row <= to.row && row - _dropped < _scrollback.size();
         ++row) {
      const std::string &line = _scrollback[row - _dropped].text;
      const std::size_t first = row == from.row ? std::min(from.column, line.size()) : 0;
      const std::size_t end =
          row == to.row ? std::min(to.column, line.size()) : line.size();
      if (row > std::max(from.row, _dropped))
        text += '\n';
      text.append(line, first, end - std::min(first, end));
    }
    return text;
  }

  bool focused() const {
    return _typed->focus == _name;
  }

  // Sends a line to the view it names, else to the one lines go to, and returns what its
  // command answers; the line may host a view or close one, which the next line follows.
  std::string run(VP::Cook &frame, std::string_view line) {
    std::string answer = frame.commands().send(address(line));
    current(followed(frame.commands().send(": view list"), _presented));
    return answer;
  }

  std::string address(std::string_view line) const {
    return _view.empty() || addressed(line) ? std::string(line)
                                            : std::format("{}: {}", _view, line);
  }

  std::string prompt() const {
    return _folder + std::string(prompt_end);
  }

  // As the area, the font and the prompt are now, the text clear of the scroll bars.
  Grid grid() const {
    const glm::ivec2 size(_area->extent);
    const glm::ivec2 cell(_font->cell);
    Grid grid{.columns = cell.x == 0 ? 0 : (size.x - 2 * margin - Scroll::width) / cell.x,
              .left = _area->offset.x + margin,
              .line = _area->offset.y + size.y - margin - cell.y};
    grid.rule = grid.line - margin;
    grid.bottom = grid.rule - Scroll::width - margin;
    grid.above = grid.bottom - _area->offset.y - margin;
    grid.room =
        grid.columns > static_cast<std::int32_t>(_folder.size() + prompt_end.size()) &&
        grid.above >= cell.y;
    return grid;
  }

  // From where vulpen started, as a shell names a folder; the host has no name to show.
  void current(std::string_view hosted) {
    const std::size_t space = hosted.find(' ');
    _view = hosted.substr(0, space);
    _folder = space == std::string_view::npos
                  ? std::string()
                  : std::filesystem::path(hosted.substr(space + 1))
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
  // sent before, completes the word at the caret, or with control copies or pastes;
  // escape lets go of the selection, and with none of the focus.
  void edit(VP::Cook &frame, const VP::Event &event) {
    if (event.kind == VP::Event::Kind::text) {
      type(event.name);
      return;
    }
    if (event.kind == VP::Event::Kind::key && event.name == "control")
      _control = event.down;
    if (event.kind != VP::Event::Kind::key || !event.down)
      return;
    const std::string_view key = event.name;
    if (key == "tab")
      return complete_word(frame);
    if (key == "escape" && _anchor == _end)
      frame.commands().send("focus"); // the next escape reaches what takes the rest
    else if (key == "escape")
      _anchor = _end;
    if (_control && key == "c" && _anchor != _end)
      frame.input().copy(selected());
    else if (_control && key == "v")
      paste(frame, frame.input().clipboard());
    else if (key == "enter")
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

  // The printable characters go in at the caret; the font draws no other.
  void type(std::string_view text) {
    for (const char typed : text)
      if (typed >= VP_VIEW::first_printable && typed <= VP_VIEW::last_printable)
        _line.insert(_caret++, 1, typed);
  }

  // As a shell takes a paste: each break sends the line typed so far.
  void paste(VP::Cook &frame, std::string_view text) {
    for (std::size_t end = text.find('\n'); end != std::string_view::npos;
         end = text.find('\n')) {
      type(text.substr(0, end));
      submit(frame);
      text.remove_prefix(end + 1);
    }
    type(text);
  }

  // A blank line only shows the prompt again, as a shell's does. Either shows the newest
  // rows, so what the line answers shows.
  void submit(VP::Cook &frame) {
    const std::string line = std::exchange(_line, {});
    _caret = 0;
    _scroll.move(static_cast<float>(_scrollback.size()));
    show(prompt() + line, VP_VIEW::role("accent"));
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
  // only one candidate fits is finished, and several are listed. A placeholder, as
  // <name>, shows what may come but is never typed in. A folder is finished with no
  // blank, so Tab again lists what it holds.
  void complete_word(VP::Cook &frame) {
    const std::string_view before = std::string_view(_line).substr(0, _caret);
    const std::size_t start = before.find_last_of(' ') + 1; // 0 when there is none
    const std::string_view word = before.substr(start);
    const std::string answer = frame.commands().send(address(std::format(
        "complete {}{}", before.substr(0, start), word.empty() ? no_word : word)));
    const std::vector<std::string_view> candidates = lines_of(answer);
    std::vector<std::string_view> typeable;
    std::ranges::copy_if(
        candidates, std::back_inserter(typeable), [](std::string_view candidate) {
          return !placeholder(candidate);
        });
    // Read before the line changes, since a word not completed points into it.
    const std::string_view shared = typeable.empty() ? word : shared_start(typeable);
    const std::size_t typed = shared.size(); // the word at the caret, as completed
    const std::size_t name = shared.rfind('/') + 1; // 0 when it names no folder
    const bool folder = shared.ends_with('/');
    _line.insert(_caret, shared.substr(word.size()));
    _caret += typed - word.size();
    if (candidates.size() == 1 && typeable.size() == 1) {
      if (!folder)
        _line.insert(_caret++, 1, ' ');
      return;
    }
    if (!candidates.empty())
      list(candidates, name);
  }

  // As a shell lists them, under the line as typed, which shows the newest rows: in
  // columns down, then across, each as wide as the widest and a gap, as many as the rows
  // hold. A path's show from where its folder ends, a placeholder whole.
  void list(std::span<const std::string_view> candidates, std::size_t folder) {
    const auto shown = [&](std::string_view candidate) {
      return placeholder(candidate) ? candidate : candidate.substr(folder);
    };
    std::size_t widest = 0;
    for (const std::string_view candidate : candidates)
      widest = std::max(widest, shown(candidate).size());
    const std::size_t width = widest + column_gap.size();
    const auto room = static_cast<std::size_t>(std::max(grid().columns, 0));
    const std::size_t across =
        std::max<std::size_t>(1, (room + column_gap.size()) / width);
    const std::size_t down = (candidates.size() + across - 1) / across;
    _scroll.move(static_cast<float>(_scrollback.size()));
    show(prompt() + _line, VP_VIEW::role("accent"));
    for (std::size_t row = 0; row < down; ++row) {
      std::string text;
      for (std::size_t at = row; at < candidates.size(); at += down) {
        const std::string_view name = shown(candidates[at]);
        text.append(name);
        if (at + down < candidates.size())
          text.append(width - name.size(), ' ');
      }
      show(text, VP_VIEW::role("text"));
    }
  }

  // Each line of the text a row, the oldest dropped past the scrollback's room, which may
  // take the widest along.
  void show(std::string_view text, std::uint32_t role) {
    const std::size_t dropped = _dropped;
    for (const std::string_view line : lines_of(text)) {
      _scrollback.push_back({std::string(line), role});
      _widest = std::max(_widest, line.size());
    }
    for (; _scrollback.size() > scrollback_rows; ++_dropped)
      _scrollback.pop_front();
    if (_dropped != dropped) {
      _widest = 0;
      for (const Shown &row : _scrollback)
        _widest = std::max(_widest, row.text.size());
    }
    fit(_dropped - dropped);
  }

  // The scroll, as the scrollback and the area are now. While the newest rows showed,
  // they still do, as a shell's do while its output comes; else rows dropped from the
  // front move it by as many, so the rows shown stay.
  void fit(std::size_t dropped) {
    const bool newest = _scroll.at_end();
    const Grid grid = this->grid();
    const std::size_t shown = grid.room ? rows_shown(grid) : 0;
    _scroll.fit(_scrollback.size(), shown, _scrollback.size() - shown);
    _scroll.move(newest ? static_cast<float>(_scrollback.size())
                        : -static_cast<float>(dropped));
    const auto columns = grid.room ? static_cast<std::size_t>(grid.columns) : 0;
    _columns.fit(_widest, columns, _widest - std::min(_widest, columns));
  }

  // The rows above the rule, down whose right edge and across whose bottom edge the
  // scroll bars run.
  VP_VIEW::Rect rows_area(const Grid &grid) const {
    const auto above =
        static_cast<std::uint32_t>(std::max(0, grid.rule - _area->offset.y));
    return {.offset = _area->offset, .extent = {_area->extent.x, above}};
  }

  // The scrollback fills the area down to the line typed, its newest row last, as a
  // shell's does. A row wider than the area is cut, and the line typed scrolls to keep
  // the caret in view.
  void lay_out(VP::Cook &frame) {
    const VP_VIEW::Rect &area = *_area;
    const glm::ivec2 cell(_font->cell);
    const auto [columns, left, line, rule, bottom, above, room] = grid();
    if (!room) {
      frame.write(_rects, 0);
      frame.write(_labels, 0);
      frame.write(_characters, 0);
      return;
    }
    const std::string start = prompt();
    const auto prompt_columns = static_cast<std::int32_t>(start.size());
    const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
    rects[0] = {.offset = area.offset,
                .extent = area.extent,
                .role = VP_VIEW::role("background")};
    rects[1] = {.offset = {area.offset.x, rule},
                .extent = {area.extent.x, rule_width},
                .role = VP_VIEW::role("border")};
    std::uint32_t count = 2;
    Labels text{frame.write(_labels), frame.write(_characters)};
    const std::size_t shown = rows_shown(grid());
    const glm::uvec2 row_extent(static_cast<std::uint32_t>(columns * cell.x), cell.y);
    const std::size_t first = _columns.first();
    for (std::size_t row = 0; row < shown; ++row) {
      const std::size_t index = _scroll.first() + row;
      const Shown &kept = _scrollback[index];
      const auto from_bottom = static_cast<std::int32_t>(shown - row) * cell.y;
      const glm::ivec2 at{left, bottom - from_bottom};
      if (const std::optional<VP_VIEW::Rect> selection =
              select(_dropped + index, kept.text.size(), at, columns, cell))
        rects[count++] = *selection;
      const std::string_view whole = kept.text;
      const std::size_t from = std::min(first, whole.size());
      text.add({.offset = at, .extent = row_extent, .role = kept.role},
               whole.substr(from, static_cast<std::size_t>(columns)));
    }
    const VP_VIEW::Rect caret =
        lay_out_line(text, start, left, columns - prompt_columns, cell, line);
    // The caret shows only in focus, so it marks where typing goes.
    if (focused())
      rects[count++] = caret;
    const VP_VIEW::Rect region = rows_area(grid());
    for (const Scroll *scroll : {&_scroll, &_columns})
      if (const std::optional<VP_VIEW::Rect> thumb = scroll->thumb(scroll->track(region)))
        rects[count++] = *thumb;
    frame.write(_rects, count);
    frame.write(_labels, text.label_count);
    frame.write(_characters, text.character_count);
  }

  // Behind what is selected of a row shown, cut at the columns the area shows.
  std::optional<VP_VIEW::Rect> select(std::size_t row,
                                      std::size_t size,
                                      glm::ivec2 at,
                                      std::int32_t columns,
                                      glm::ivec2 cell) const {
    const auto [from, to] = std::minmax(_anchor, _end);
    if (from == to || row < from.row || row > to.row)
      return std::nullopt;
    const std::size_t left = _columns.first();
    const std::size_t first = std::max(row == from.row ? from.column : 0, left);
    const std::size_t end = std::min(row == to.row ? to.column : size,
                                     left + static_cast<std::size_t>(columns));
    if (first >= end)
      return std::nullopt;
    return VP_VIEW::Rect{
        .offset = at + glm::ivec2(static_cast<std::int32_t>(first - left) * cell.x, 0),
        .extent = glm::uvec2(static_cast<std::uint32_t>(end - first) * cell.x, cell.y),
        .role = VP_VIEW::role("border")};
  }

  // The prompt and the line typed, scrolled so the caret stays in view; returns the
  // caret.
  VP_VIEW::Rect lay_out_line(Labels &text,
                             std::string_view start,
                             std::int32_t left,
                             std::int32_t room,
                             glm::ivec2 cell,
                             std::int32_t line) {
    const auto columns = static_cast<std::size_t>(room);
    _first = _caret < columns ? 0 : _caret - columns + 1;
    const std::int32_t typed = left + static_cast<std::int32_t>(start.size()) * cell.x;
    text.add({.offset = {left, line},
              .extent = glm::uvec2(static_cast<std::uint32_t>(typed - left), cell.y),
              .role = VP_VIEW::role("accent")},
             start);
    text.add({.offset = {typed, line},
              .extent = glm::uvec2(static_cast<std::uint32_t>(room * cell.x), cell.y),
              .role = VP_VIEW::role("text")},
             std::string_view(_line).substr(_first, columns));
    const auto caret = static_cast<std::int32_t>(_caret - _first);
    return {.offset = {typed + caret * cell.x, line},
            .extent = {caret_width, static_cast<std::uint32_t>(cell.y)},
            .role = VP_VIEW::role("accent")};
  }

  void command(VP::Call &call) override {
    if (call.is(_help))
      help(call);
    else if (call.is(_complete))
      complete(call);
    else if (call.is(_clear) && _window)
      clear();
    else if (call.is(_clear))
      call.reply(clear_screen);
  }

  // Its rows dropped as the oldest are, so a selection made before selects nothing.
  void clear() {
    _dropped += _scrollback.size();
    _scrollback.clear();
    _widest = 0;
    _anchor = _end;
    fit(0);
  }

  // Every command as a tree of the words they start with: a word that more than one
  // starts with once, their usages under it without it, indented, and so again for a
  // second word, as input key; a command named as its group, as quit beside quit force,
  // is the group's row. In the order of their words, so a group reads in one place, and
  // what each does in one column.
  static void help(VP::Call &call) {
    std::vector<VP::Usage> usages = call.commands().usages();
    std::ranges::sort(usages, {}, &VP::Usage::usage);
    std::map<std::string_view, std::size_t> begun; // usages a head starts, by its words
    for (const VP::Usage &usage : usages)
      for (const std::string_view head : heads_of(usage.usage))
        ++begun[head];
    std::vector<std::pair<std::string, std::string_view>> rows; // indented, and help
    std::set<std::string_view> opened;
    for (const VP::Usage &usage : usages) {
      std::size_t depth = 0;
      std::size_t from = 0; // where its words past the heads it is under start
      bool own = false;     // the usage is its group's row
      for (const std::string_view head : heads_of(usage.usage)) {
        if (begun[head] < 2)
          break;
        if (opened.insert(head).second) {
          own = head.size() == usage.usage.size();
          rows.emplace_back(std::string(2 * depth, ' ').append(head.substr(from)),
                            own ? usage.help : "");
        }
        from = head.size() + 1; // past the blank after it
        ++depth;
      }
      if (!own)
        rows.emplace_back(std::string(2 * depth, ' ').append(
                              usage.usage.substr(std::min(from, usage.usage.size()))),
                          usage.help);
    }
    std::size_t width = 0;
    for (const auto &[words, help] : rows)
      if (!help.empty())
        width = std::max(width, words.size());
    for (const auto &[words, help] : rows)
      call.reply(help.empty() ? words
                              : std::string(words)
                                    .append(width - words.size(), ' ')
                                    .append(column_gap)
                                    .append(help));
  }

  // The words a usage starts with that may head a group: its first and its first two,
  // unless one is a placeholder.
  static std::vector<std::string_view> heads_of(std::string_view usage) {
    std::vector<std::string_view> heads;
    for (const std::string_view word : words_of(usage)) {
      if (heads.size() == help_depth || placeholder(word))
        break;
      heads.push_back(usage.substr(0, word.data() + word.size() - usage.data()));
    }
    return heads;
  }

  static void complete(VP::Call &call) {
    const std::span<const std::string_view> typed = call.arguments();
    const std::string_view begun = typed.back() == no_word ? "" : typed.back();
    std::set<std::string> found; // sorted, and each once
    for (const VP::Usage &usage : call.commands().usages()) {
      std::vector<std::string_view> words = words_of(usage.usage);
      if (kind_of(words.back()).more && words.size() < typed.size())
        words.resize(typed.size(), words.back());
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
      for (const std::string &word : placeholder(next) ? names(next, call, typed, begun)
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
  // As the frame before showed them: the view presented, and the one hosted last.
  std::string _presented;
  std::string _newest;
  bool _followed = false; // until the first frame finds the view lines go to
  std::vector<std::string> _kept; // the views the log keeps to, as it was told
  bool _prompted = false;
  bool _window = false;
  // In a window: what it reads and writes, and the line being typed.
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  std::string _name;    // as the focus names it
  glm::vec2 _pointer{}; // as the events so far left it
  std::string _title;
  VP_VIEW::Items *_tabs = nullptr; // the one a panel shows for it
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels;
  VP::Upload<VP_VIEW::Character> _characters;
  std::deque<Shown> _scrollback;
  std::size_t _dropped = 0; // rows the scrollback dropped from its front, or clear took
  std::size_t _widest = 0;  // its widest row's bytes
  Scroll _scroll;           // the rows of it shown
  Scroll _columns{Scroll::Axis::across};
  Mark _anchor;             // where a drag over the scrollback started
  Mark _end;                // where it is now; the anchor itself for no selection
  bool _selecting = false;  // from a press on the scrollback until it is let go
  bool _control = false;    // held, so c copies the selection and v pastes
  bool _sideways = false;   // shift held, as the window says, so the wheel scrolls across
  std::string _line;
  std::size_t _caret = 0;
  std::size_t _first = 0; // the first character of the line shown
  std::vector<std::string> _history;
  std::size_t _recalled = 0; // the line recalled; past the history for a new one
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<CommandLine>("CommandLine");
}
