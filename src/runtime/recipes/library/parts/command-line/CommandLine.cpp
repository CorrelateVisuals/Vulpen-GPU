// Longer than a file should be (CPP12): the line typed, its completion, its history and
// the rows above it are one part's job, and a part's class stays in one file's unnamed
// namespace, so two copies of the part in one binary never clash.
#include "contracts/Font.h"
#include "contracts/Handed.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Place.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Caret.h"
#include "shared/Labels.h"
#include "shared/Params.h"
#include "shared/Scroll.h"
#include "shared/Words.h"

#include <glm/common.hpp>

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
constexpr std::string_view find_prompt = "find: "; // the line a search's words are typed on
// The command a find bar sends its words to, unless its finds param names another.
constexpr std::string_view find_command = "find";
// What parts a typed path into folders: Windows takes \ beside /.
constexpr std::string_view separators =
    std::filesystem::path::preferred_separator == '/' ? "/" : "/\\";

// In a window: what it keeps, and how it lays out, in pixels.
constexpr std::size_t scrollback_rows = 1000; // the oldest go first (A03)
constexpr std::size_t history_lines = 100;
// The edits of the line typed that control with z takes back, the oldest going past them
// (A03); and that key, which with shift too makes one again.
constexpr std::size_t undo_room = 100;
constexpr std::string_view undo_key = "z";
constexpr std::int32_t margin = 4; // around the text
constexpr std::uint32_t rule_width = 1;
constexpr std::uint32_t label_room = 256;
constexpr std::size_t rows_most = label_room - 2; // the prompt and the line take two
// The ground, the rule above the line, the line selected whole, the caret, the scroll
// bars' thumbs, and the selection a row shown.
constexpr std::uint32_t rect_room = 6 + rows_most;
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

// Where a line one word short of its usage lacks a node, or a view hosted: that word's
// place among the line's words, the first the usage may not leave out being of that
// kind; none for any other line. The usage is the one whose command words start the
// line, the longest, as the command port picks it.
struct Wanted {
  std::size_t at = 0;
  bool view = false;
};

std::optional<Wanted> wanted(std::span<const VP::Usage> usages,
                             std::span<const std::string_view> words) {
  std::vector<std::string_view> best;
  std::size_t named = 0; // its command words
  for (const VP::Usage &usage : usages) {
    std::vector<std::string_view> its = words_of(usage.usage);
    const auto count =
        static_cast<std::size_t>(std::ranges::find_if(its, placeholder) - its.begin());
    if (count <= named || count > words.size() ||
        !std::ranges::equal(std::span(its).first(count), words.first(count)))
      continue;
    best = std::move(its);
    named = count;
  }
  std::size_t needed = best.size() - named;
  if (needed > 0 && best.back().starts_with('['))
    --needed;
  if (named == 0 || words.size() - named + 1 != needed)
    return std::nullopt;
  for (std::size_t at = named; at < named + needed; ++at)
    if (const std::string_view kind = kind_of(best[at]).name;
        kind == "<node>" || kind == "<view>")
      return Wanted{at, kind == "<view>"};
  return std::nullopt;
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

// The folders and files where a path begun points, each as the whole word, from where the
// line asking counts, as its <file> would resolve: the folder a person typed it at. A
// name with a blank would split into two words, so it is left out, and a hidden one shows
// once the name begun starts with a dot, as in a shell. A folder that cannot be listed,
// as a file followed by /, is refused by the file port, naming why.
std::vector<std::string> paths(VP::Call &call, std::string_view begun) {
  const std::string_view folder = begun.substr(0, begun.find_last_of(separators) + 1);
  const bool hidden = begun.substr(folder.size()).starts_with('.');
  std::vector<std::string> paths;
  for (const std::string &name : call.files().list(call.resolved(folder)))
    if ((hidden || !name.starts_with('.')) && name.find(' ') == std::string::npos)
      paths.push_back(std::string(folder).append(name));
  return paths;
}

// The names of the files the view holds, as a word naming no folder names one: its
// nodes', as it runs, each recipe unfolded. A name with a blank is left out, as a path's
// is.
std::vector<std::string> held(VP::Call &call) {
  std::vector<std::string> names;
  for (const VP::Node &node : call.unfolded().nodes)
    for (const std::string &file : node.files)
      if (file.find(' ') == std::string::npos)
        names.push_back(file);
  std::ranges::sort(names);
  names.erase(std::ranges::unique(names).begin(), names.end());
  return names;
}

// Every view hosted, at any depth, by the name a line addresses it with: those the view
// vulpen started with hosts, then those each of them hosts, each once.
void hosted(VP::Call &call, std::string_view host, std::vector<std::string> &names) {
  if (const VP::View *const view = call.view(host))
    for (const VP::Child &child : view->children)
      if (std::ranges::find(names, child.name) == names.end()) {
        names.push_back(child.name);
        hosted(call, child.name, names);
      }
}

// The library's recipes, as recipe drop names them, past the kind recipe list lists each
// under; none where no part lists them.
std::vector<std::string> recipes(VP::Call &call) {
  std::vector<std::string> names;
  try {
    const std::string listed = call.commands().send("recipe list");
    for (const std::string_view line : lines_of(listed))
      if (!line.empty())
        names.emplace_back(line.substr(line.rfind('/') + 1));
  } catch (const std::runtime_error &) {
  }
  return names;
}

// What a placeholder stands for: the view's nodes or connections, the views hosted, the
// params of the node named just before a key, the recipes of the library, the paths a word
// begun may become, or, for a word naming no folder, the files the view holds. Any other
// answers as itself.
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
  } else if (kind == "<view>") {
    hosted(call, {}, names);
  } else if (kind == "<recipe>") {
    names = recipes(call);
  } else if (kind == "<file>" || kind == "<project>" ||
             (kind == "<path>" && std::filesystem::path(begun).has_parent_path())) {
    names = paths(call, begun);
  } else if (kind == "<path>") {
    names = held(call);
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

// A line typed, as an edit found it: its text and where its caret was.
struct Line {
  std::string text;
  std::size_t caret = 0;
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
// typed or piped in runs, what it answers is printed, and the run ends with the input. In
// a window it takes keys from the keys part while it has the focus, which a press in its
// area gives it, and shows what lines answer and the log above the line typed, in the
// Rect its area gives, with a tab for the panel it sits in, named by its title param; a
// drag selects in what it shows, as a double click does a word and one more press its
// row, which control with c copies, and the wheel or a drag on the bar at its right edge
// scrolls it; turned sideways or with shift, or a drag on the bar above the rule, scrolls
// its rows across, while the line typed keeps its caret in view. A press on the line
// typed puts the caret there, and a drag over it, shift with a key that moves the caret,
// or control with a, selects in it, which control with c copies, with x cuts, and what is
// typed or pasted replaces. Only the one on the terminal registers help, complete and
// clear, as a command has one owner, and a terminal in a window sends them as it sends
// any line, taking what clear answers as a terminal takes it; on the terminal, the end of
// the input ends the run while no window is open, as one a launcher gives no input still
// shows its window. In a window, control+f makes its line a search, the line typed set
// aside: the words, which enter and control+f find again in the rows above, older first,
// selecting the match and scrolling to it, until escape gives the line back. As a find
// bar, on=find, it is a search alone, over a host whose control+f opens it: a row at the
// top of its area, whose words enter and control+f send to the command its finds param
// names, its words joined by commas, as the graph's select,find, or to find, which the
// editors' owner answers, and which escape closes, the focus back to the host, and a
// press on which takes the focus back from the host; the rest of its area goes to the
// host. A line that names no view goes to the current view, which view switch moves, so
// it needs no name and every log line still has one. The prompt shows that view's folder,
// as a shell shows the one it is in, where a line's relative paths count from, and the
// log keeps to that view. A line one word short of the node its usage wants acts on the
// nodes the graph selects, a line each, as Tab with no word begun offers them.
class CommandLine final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string on = node.param<std::string>("on");
    check_word("on", on, {"terminal", "window", "find"});
    _window = on != "terminal";
    _finder = on == "find";
    if (!_window) {
      _help = node.command(
          "help", "lists every command, under the words it shares, and what each does");
      _complete = node.command(
          "complete <value>...",
          "lists the words that may come next, the last word given being the start of "
          "one, or \"\" for one not begun");
      _clear = node.command("clear", "clears the terminal");
      return;
    }
    _area = &node.input<VP_VIEW::Rect>("area");
    _font = &node.input<VP_VIEW::Font>("font");
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _name = node.name();
    _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
    _labels = node.upload<VP_VIEW::Label>("labels", label_room);
    _characters = node.upload<VP_VIEW::Character>("characters", character_room);
    if (_finder) {
      _host = node.param<std::string>("host");
      _finds = node.sets("finds") ? node.param<std::string>("finds")
                                  : std::string(find_command);
      std::ranges::replace(_finds, ',', ' ');
      _own_view = node.view_name();
      _rest = &node.output<VP_VIEW::Rect>("rest");
      return;
    }
    _title = node.param<std::string>("title");
    _tabs = &node.output<VP_VIEW::Items>("tabs");
    // A graph's place, which the prompt shows, and the lines a menu hands it, when a
    // connection brings them; the lines handed before it bound are not taken again.
    _place = reached(node, "place") ? &node.input<VP_VIEW::Place>("place") : nullptr;
    _handed = reached(node, "handed") ? &node.input<VP_VIEW::Handed>("handed") : nullptr;
    _taken = _handed ? _handed->count : 0;
    _followed = false;
    name_tab();
  }

  // Whether a connection reaches a port of its, as one it may go without.
  bool reached(VP::Bind &node, std::string_view port) const {
    return std::ranges::any_of(
        node.view().connections, [&](const VP::Connection &connection) {
          return std::ranges::any_of(connection.to, [&](const VP::Endpoint &to) {
            return to.node == _name && to.port == port;
          });
        });
  }

  void cook(VP::Cook &frame) override {
    if (_finder)
      return find_bar(frame);
    follow(frame);
    keep_log(frame);
    if (_window)
      in_window(frame);
    else
      on_terminal(frame);
  }

  // The log keeps to the view lines go to and the views it hosts, so with views running
  // side by side, the terminal vulpen was started from and the rows here show the one
  // worked on, and every view's problems; told only when they change, so a frame
  // allocates nothing for them.
  void keep_log(VP::Cook &frame) {
    const VP::View *const followed = _view.empty() ? nullptr : frame.view(_view);
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

  // Lines go to the command port's current view, which view switch moves, so both
  // terminals, the graph and the Perform panel follow one; found again once it changed,
  // as a line typed may change it. The prompt names its folder from where vulpen
  // started, as a shell names one, or where a graph is, as a connection to its place
  // brings it, the folder shown or a node's whose inside shows; the view vulpen started
  // with has none to show. A line typed counts its relative paths from that folder, as a
  // shell does, and with none from where vulpen started.
  void follow(VP::Cook &frame) {
    const std::string_view now = frame.commands().current();
    const bool moved = _place && _place->folder != _placed;
    if (_followed && now == _view && !moved)
      return;
    _followed = true;
    _view = now;
    if (_place)
      _placed = _place->folder;
    const VP::View *const view = frame.view(_view);
    const std::filesystem::path folder =
        !_placed.empty()           ? std::filesystem::path(_placed)
        : _view.empty() || !view ? std::filesystem::path()
                                 : view->file.parent_path();
    _folder = folder.empty() ? std::string()
                             : folder.lexically_proximate(std::filesystem::current_path())
                                   .generic_string();
    name_tab();
  }

  void on_terminal(VP::Cook &frame) {
    VP::TerminalPort &terminal = frame.terminal();
    if (terminal.typed())
      return typing(frame, terminal);
    const std::span<const std::string> lines = terminal.lines();
    for (const std::string &line : lines)
      for (const std::string &each : filled(frame, line))
        if (const std::string answer = run(frame, each); !answer.empty())
          terminal.print(answer);
    if (terminal.ended() && !frame.input().windowed()) {
      terminal.prompt(
          "\n"); // ends the prompt's line, as a shell does at the end of input
      frame.commands().send("quit");
    } else if (!lines.empty() || !_prompted) {
      terminal.prompt(prompt());
      _prompted = true;
    }
  }

  // A person at the terminal: each key edits the line as in a window, and the line is
  // drawn again after the keys, and after the log, which prints in its place. control+c
  // ends the run, as its signal did, and control+d on an empty line, as the end of input
  // does.
  void typing(VP::Cook &frame, VP::TerminalPort &terminal) {
    _terminal = &terminal;
    const std::span<const VP::Event> keys = terminal.keys();
    bool ends = terminal.ended();
    for (const VP::Event &event : keys) {
      const bool empty = _line.empty();
      edit(frame, event);
      ends = ends || (_control && event.down &&
                      (event.name == "c" || (event.name == "d" && empty)));
    }
    if (ends)
      frame.commands().send("quit");
    else if (!keys.empty() || !frame.commands().log().empty() || !_prompted)
      terminal.line(prompt() + _line, prompt().size() + _caret);
    _prompted = true;
    _terminal = nullptr;
  }

  // The log since the frame before and the keys pressed, then all of it drawn anew. With
  // no room it takes no keys, so a line never runs where nobody sees it typed. A key let
  // go while another part had the focus never came, so out of focus control counts as up.
  void in_window(VP::Cook &frame) {
    fit(0);
    for (const VP::Logged &line : frame.commands().log())
      show(line.text, role_of(line));
    take(frame);
    point(frame);
    if (_selecting)
      drift();
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
        if (_line_pressed)
          drag_line();
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
          _line_pressed.reset();
        }
        if (event.down && inside && grid.room &&
            (_scroll.press(_pointer, _scroll.track(rows)) ||
             _columns.press(_pointer, _columns.track(rows))))
          continue;
        if (event.down && VP_VIEW::contains(_line_row, _pointer))
          press_line(event.presses);
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

  // Each frame a drag selects with the pointer past the rows or the columns shown, they
  // scroll on, as the editor's do, and the selection with them.
  void drift() {
    const Grid grid = this->grid();
    const std::size_t shown = rows_shown(grid);
    if (!grid.room || shown == 0)
      return;
    const glm::vec2 cell(_font->cell);
    const auto top = static_cast<float>(grid.bottom) - static_cast<float>(shown) * cell.y;
    const glm::vec2 from(static_cast<float>(grid.left), top);
    const glm::vec2 room(static_cast<float>(grid.columns), static_cast<float>(shown));
    const glm::vec2 cells = (_pointer - from) / cell;
    const glm::vec2 past = glm::min(cells, 0.0f) + glm::max(cells - room, 0.0f);
    if (past == glm::vec2(0.0f))
      return;
    _scroll.drift(past.y);
    _columns.drift(past.x);
    _end = mark_at(grid);
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

  // Sends a line to the view it names, else to the current one, and returns what its
  // command answers; the line may switch the current view, which the next line follows.
  std::string run(VP::Cook &frame, std::string_view line) {
    std::string answer = frame.commands().send_typed(address(line), _folder);
    follow(frame);
    // As the port answers it, a line, with the line break a reply ends with.
    if (_window && answer.starts_with(clear_screen)) {
      clear();
      return {};
    }
    return answer;
  }

  std::string address(std::string_view line) const {
    return _view.empty() || addressed(line) ? std::string(line)
                                            : std::format("{}: {}", _view, line);
  }

  // Whether a line names a view other than the one lines go to, whose selection the
  // graph does not show.
  bool elsewhere(std::string_view line) const {
    const std::vector<std::string_view> words = words_of(line);
    return !words.empty() && words.front().ends_with(':') &&
           words.front().substr(0, words.front().size() - 1) != _view;
  }

  // What the graph has selected in the view lines go to, as selected lists it; none where
  // no part selects.
  static std::vector<std::string> selection(VP::Cook &frame,
                                            std::span<const VP::Usage> usages,
                                            const std::string &asked) {
    if (std::ranges::find(usages, "selected", &VP::Usage::usage) == usages.end())
      return {};
    std::vector<std::string> names;
    for (const std::string_view name : lines_of(frame.commands().send(asked)))
      names.emplace_back(name);
    return names;
  }

  // The lines a line typed runs: itself, or, one word short of a node its usage wants,
  // or a view hosted, a line for each the graph selects of that kind, its name where the
  // usage wants it, as the graph's delete sends a line each; so the lines shown and kept
  // name what they act on (V08).
  std::vector<std::string> filled(VP::Cook &frame, std::string_view line) const {
    std::vector<std::string_view> words = words_of(line);
    const VP::View *const view = frame.view(_view);
    if (elsewhere(line) || !view)
      return {std::string(line)};
    const std::string named = addressed(line) && !words.empty()
                                  ? std::string(words.front()).append(" ")
                                  : std::string();
    if (!named.empty())
      words.erase(words.begin());
    const std::vector<VP::Usage> usages = frame.commands().usages();
    const std::optional<Wanted> slot = wanted(usages, words);
    if (!slot)
      return {std::string(line)};
    std::vector<std::string> lines;
    for (const std::string &name : selection(frame, usages, address("selected"))) {
      if (slot->view ? std::ranges::find(view->children, name, &VP::Child::name) ==
                           view->children.end()
                     : std::ranges::find(view->nodes, name, &VP::Node::name) ==
                           view->nodes.end())
        continue;
      std::string each = named;
      for (std::size_t at = 0; at <= words.size(); ++at) {
        if (at == slot->at)
          each.append(name).append(" ");
        if (at < words.size())
          each.append(words[at]).append(" ");
      }
      each.pop_back();
      lines.push_back(std::move(each));
    }
    if (lines.empty())
      lines.emplace_back(line);
    return lines;
  }

  std::string prompt() const {
    return _finding ? std::string(find_prompt) : _folder + std::string(prompt_end);
  }

  // As the area, the font and the prompt are now, the text clear of the scroll bars, and
  // of a strip as wide as a bar at its left, so it sits as far in from either side.
  Grid grid() const {
    const glm::ivec2 size(_area->extent);
    const glm::ivec2 cell(_font->cell);
    Grid grid{.columns =
                  cell.x == 0 ? 0 : (size.x - 2 * margin - 2 * Scroll::width) / cell.x,
              .left = _area->offset.x + Scroll::width + margin,
              .line = _area->offset.y + size.y - margin - cell.y};
    grid.rule = grid.line - margin;
    grid.bottom = grid.rule - Scroll::width - margin;
    grid.above = grid.bottom - _area->offset.y - margin;
    grid.room =
        grid.columns > static_cast<std::int32_t>(_folder.size() + prompt_end.size()) &&
        grid.above >= cell.y;
    return grid;
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

  // Control with z takes the line's last edit back, and with shift too makes it again;
  // any other event edits the line, and an edit that changed it is kept to take back,
  // the characters typed joining those typed just before them while the caret stayed
  // where those left it and no blank starts a word, so a word goes back at once. A line
  // sent, or a search begun or ended, starts the edits anew.
  void edit(VP::Cook &frame, const VP::Event &event) {
    if (event.kind == VP::Event::Kind::key && event.name == "shift")
      _shift = event.down;
    if (event.kind == VP::Event::Kind::key && event.down && _control &&
        event.name == undo_key) {
      undo(_shift);
      return;
    }
    Line before{_line, _caret};
    const std::size_t sent = _sent;
    const bool finding = _finding;
    change(frame, event);
    if (_sent != sent || _finding != finding) {
      _undone.clear();
      _redone.clear();
      _typing = false;
      return;
    }
    if (_line == before.text)
      return;
    const bool typed = event.kind == VP::Event::Kind::text;
    const bool joins =
        typed && _typing && before.caret == _typed_to && !event.name.starts_with(' ');
    _typing = typed;
    _typed_to = _caret;
    if (joins)
      return;
    _undone.push_back(std::move(before));
    if (_undone.size() > undo_room)
      _undone.erase(_undone.begin());
    _redone.clear();
  }

  // The line before its last edit, or with again before the last taken back, its caret
  // where it was then.
  void undo(bool again) {
    std::vector<Line> &from = again ? _redone : _undone;
    std::vector<Line> &to = again ? _undone : _redone;
    if (from.empty())
      return;
    to.push_back({std::exchange(_line, std::move(from.back().text)), _caret});
    _caret = from.back().caret;
    from.pop_back();
    _typing = false;
    _from.reset();
  }

  // Typed text goes in at the caret, and a key edits the line, sends it, recalls one
  // sent before, completes the word at the caret, or with control copies, pastes or
  // clears, sending clear from here, so what it answers comes back here, or takes the
  // word before the caret with backspace, or after it with delete, and moves the caret
  // as far with left and right, taking nothing; escape lets go of the
  // selection, and with none of the focus. In a window, a key that moves the caret with
  // shift held selects in the line, as control with a selects all of it and a drag
  // over it does, which what is typed or pasted next replaces, backspace or delete
  // takes, and control with c copies and with x cuts; a key that types leaves it for
  // the text that follows the key, and any other lets go of it.
  void change(VP::Cook &frame, const VP::Event &event) {
    if (event.kind == VP::Event::Kind::text) {
      take_selected();
      type(event.name);
      return;
    }
    if (event.kind == VP::Event::Kind::key && event.name == "control")
      _control = event.down;
    if (event.kind != VP::Event::Kind::key || !event.down || modifier(event.name))
      return;
    const std::string_view key = event.name;
    if (_window && _shift && stretch(key))
      return;
    if (_from && selection_taken(frame, key))
      return;
    if (_control || !types(key))
      _from.reset();
    if (_control && key == "a" && _window) {
      if (!_line.empty())
        _from = 0;
      _anchor = _end; // a copy takes the line, not the rows above
      _caret = _line.size();
      return;
    }
    if ((_finding || _finder) && finds(frame, key))
      return;
    if (_control && key == "f" && _window) {
      _finding = true;
      _set_aside = std::exchange(_line, _words);
      _caret = _line.size();
      _found.reset();
      return;
    }
    if (key == "tab")
      return complete_word(frame);
    if (key == "escape" && _window && _anchor == _end)
      frame.commands().send("focus"); // the next escape reaches what takes the rest
    else if (key == "escape")
      _anchor = _end;
    if (_control && key == "c" && _anchor != _end)
      frame.input().copy(selected());
    else if (_control && key == "v")
      paste(frame, frame.input().clipboard());
    else if (_control && key == "l")
      show(run(frame, "clear"), VP_VIEW::role("text"));
    else if (key == "enter")
      submit(frame);
    else if (key == "backspace" && _caret > 0) {
      const std::size_t from = _control ? word_before(_line, _caret) : _caret - 1;
      _line.erase(from, std::exchange(_caret, from) - from);
    }
    else if (key == "delete" && _caret < _line.size())
      _line.erase(_caret, _control ? word_after(_line, _caret) - _caret : 1);
    else if (const std::optional<std::size_t> to = moved(key))
      _caret = *to;
    else if (key == "up" || key == "down")
      recall(key == "up");
  }

  // Where left and right move the caret, a character over or with control a word, and
  // home and end, to the line's start and its end; none for any other key.
  std::optional<std::size_t> moved(std::string_view key) const {
    if (key == "left")
      return _caret == 0 ? 0 : _control ? word_before(_line, _caret) : _caret - 1;
    if (key == "right")
      return _caret == _line.size() ? _caret
             : _control             ? word_after(_line, _caret)
                                    : _caret + 1;
    if (key == "home" || key == "end")
      return key == "home" ? 0 : _line.size();
    return std::nullopt;
  }

  // A key that moves the caret, with shift held, selects from where the caret was, or
  // where the selection started; whether it took the key.
  bool stretch(std::string_view key) {
    const std::optional<std::size_t> to = moved(key);
    if (!to)
      return false;
    if (!_from)
      _from = _caret;
    _caret = *to;
    if (*_from == _caret)
      _from.reset();
    return true;
  }

  // The window names a key that types by the character it types, or space, and the text
  // follows it as an event of its own.
  static bool types(std::string_view key) {
    return key.size() == 1 || key == "space";
  }

  // The keys a selection of the line takes: backspace and delete take it, control with c
  // copies it, with x cuts it and with v pastes in its place, escape lets go of it, as
  // the editor's does, and left goes to its start, right to its end, home and end to the
  // line's. Whether it took the key.
  bool selection_taken(VP::Cook &frame, std::string_view key) {
    const auto [from, to] = std::minmax(*_from, _caret);
    if (_control && (key == "c" || key == "x"))
      frame.input().copy(std::string_view(_line).substr(from, to - from));
    const bool cuts = _control && (key == "x" || key == "v");
    if (key == "backspace" || key == "delete" || cuts) {
      take_selected();
      if (key == "v")
        paste(frame, frame.input().clipboard());
      return true;
    }
    if (_control && key == "c")
      return true;
    if (key == "escape") {
      _from.reset();
      return true;
    }
    if (key != "left" && key != "right" && key != "home" && key != "end")
      return false;
    _from.reset();
    _caret = key == "left"    ? from
             : key == "right" ? to
             : key == "home"  ? 0
                              : _line.size();
    return true;
  }

  // The selection goes, for what takes its place.
  void take_selected() {
    if (!_from)
      return;
    const auto [from, to] = std::minmax(*std::exchange(_from, std::nullopt), _caret);
    _line.erase(from, to - from);
    _caret = from;
  }

  // The keys held while another is pressed, which change none of the line.
  static bool modifier(std::string_view key) {
    return key == "control" || key == "shift" || key == "alt" || key == "super";
  }

  // The printable characters go in at the caret; the font draws no other.
  void type(std::string_view text) {
    for (const char typed : text)
      if (typed >= VP_VIEW::first_printable && typed <= VP_VIEW::last_printable)
        _line.insert(_caret++, 1, typed);
  }

  // As a shell takes a paste: each break sends the line typed so far; a search takes its
  // first line alone, as it sends nothing.
  void paste(VP::Cook &frame, std::string_view text) {
    if (_finding || _finder)
      return type(text.substr(0, text.find('\n')));
    for (std::size_t end = text.find('\n'); end != std::string_view::npos;
         end = text.find('\n')) {
      type(text.substr(0, end));
      submit(frame);
      text.remove_prefix(end + 1);
    }
    type(text);
  }

  // A blank line only shows the prompt again, as a shell's does. Either shows the newest
  // rows, so what the line answers shows. The history keeps the line as typed, and the
  // rows each line it runs, the selection filled in.
  void submit(VP::Cook &frame) {
    ++_sent;
    const std::string line = std::exchange(_line, {});
    _caret = 0;
    _scroll.move(static_cast<float>(_scrollback.size()));
    if (line.find_first_not_of(' ') == std::string::npos)
      return show(prompt() + line, VP_VIEW::role("accent"));
    if (_history.empty() || _history.back() != line)
      _history.push_back(line);
    if (_history.size() > history_lines)
      _history.erase(_history.begin());
    _recalled = _history.size();
    for (const std::string &each : filled(frame, line)) {
      show(prompt() + each, VP_VIEW::role("accent"));
      show(run(frame, each), VP_VIEW::role("text"));
    }
  }

  // The lines a menu handed since it last looked, with room or not, as no key is pressed:
  // each run as if typed and sent, so the rows and the history show it as a person's, the
  // line being typed kept as it was; or begun in place of it, the caret at its end, for
  // the person to finish, and for a list, what may come there listed, as Tab lists it.
  void take(VP::Cook &frame) {
    if (!_handed || _handed->count == _taken)
      return;
    _taken = _handed->count;
    for (const VP_VIEW::Handed::Line &handed : _handed->lines) {
      if (handed.way != VP_VIEW::Handed::Way::run) {
        _line = handed.text;
        _caret = _line.size();
        _from.reset();
        if (handed.way == VP_VIEW::Handed::Way::list)
          complete_word(frame);
        continue;
      }
      const std::string typed = std::exchange(_line, handed.text);
      const std::size_t caret = _caret;
      submit(frame);
      _line = typed;
      _caret = caret;
    }
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
  // blank, so Tab again lists what it holds. With no word begun, the candidates the
  // graph selects are the only ones, so Tab fills in the node a line acts on; a letter
  // typed brings the rest back.
  void complete_word(VP::Cook &frame) {
    const std::string_view before = std::string_view(_line).substr(0, _caret);
    const std::size_t start = before.find_last_of(' ') + 1; // 0 when there is none
    const std::string_view word = before.substr(start);
    const std::string answer = frame.commands().send_typed(
        address(std::format(
            "complete {}{}", before.substr(0, start), word.empty() ? no_word : word)),
        _folder);
    std::vector<std::string_view> candidates = lines_of(answer);
    if (word.empty() && !elsewhere(before)) {
      const std::vector<std::string> chosen =
          selection(frame, frame.commands().usages(), address("selected"));
      const auto unchosen = [&](std::string_view candidate) {
        return std::ranges::find(chosen, candidate) == chosen.end();
      };
      if (!std::ranges::all_of(candidates, unchosen))
        std::erase_if(candidates, unchosen);
    }
    std::vector<std::string_view> typeable;
    std::ranges::copy_if(
        candidates, std::back_inserter(typeable), [](std::string_view candidate) {
          return !placeholder(candidate);
        });
    // Read before the line changes, since a word not completed points into it.
    const std::string_view shared = typeable.empty() ? word : shared_start(typeable);
    const std::size_t typed = shared.size(); // the word at the caret, as completed
    const std::size_t name = shared.find_last_of(separators) + 1; // 0 for no folder
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
    const auto room = _terminal ? _terminal->columns()
                                : static_cast<std::size_t>(std::max(grid().columns, 0));
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
    if (_terminal) { // printed in place of the line typed, which is drawn again after
      if (!text.empty())
        _terminal->print(text);
      return;
    }
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
    _line_row = {};
    if (!room) {
      frame.write(_rects, 0);
      frame.write(_labels, 0);
      frame.write(_characters, 0);
      return;
    }
    _line_row = {.offset = {area.offset.x, rule},
                 .extent = {area.extent.x,
                            static_cast<std::uint32_t>(
                                area.offset.y + static_cast<std::int32_t>(area.extent.y) -
                                rule)}};
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
    if (const auto chosen = line_selected(columns - prompt_columns, cell, line))
      rects[count++] = *chosen;
    // The caret shows only in focus, so it marks where typing goes.
    const bool typed = !_typed->to(_name).empty();
    if (focused() && _blink.shows(frame.index(), caret.offset, typed))
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

  // Behind what is selected of the line typed, as much of it as shows.
  std::optional<VP_VIEW::Rect>
  line_selected(std::int32_t room, glm::ivec2 cell, std::int32_t line) const {
    if (!_from)
      return std::nullopt;
    const auto [from, to] = std::minmax(*_from, _caret);
    const std::size_t first = std::max(from, _first);
    const std::size_t end = std::min(to, _first + static_cast<std::size_t>(room));
    if (first >= end)
      return std::nullopt;
    return VP_VIEW::Rect{
        .offset = {_line_left + static_cast<std::int32_t>(first - _first) * cell.x, line},
        .extent = glm::uvec2(static_cast<std::uint32_t>(end - first) * cell.x, cell.y),
        .role = VP_VIEW::role("border")};
  }

  // The boundary between characters of the line typed nearest a place across, as a press
  // puts the editor's caret.
  std::size_t column_at(float x) const {
    if (_font->cell.x == 0)
      return _caret;
    const float cells = std::round((x - static_cast<float>(_line_left)) /
                                   static_cast<float>(_font->cell.x));
    const auto past = static_cast<std::size_t>(std::max(cells, 0.0f));
    return std::min(_first + past, _line.size());
  }

  // A press on the line typed puts the caret where it was, and a drag from there selects
  // in the line; the second press in a row selects the word pressed, and any more the
  // line.
  void press_line(std::uint32_t presses) {
    const std::size_t at = column_at(_pointer.x);
    _from.reset();
    _caret = at;
    _line_pressed = at;
    if (presses < 2)
      return;
    _line_pressed.reset();
    const auto [from, to] =
        presses == 2 ? word_at(_line, at) : std::pair{std::size_t{0}, _line.size()};
    if (from != to)
      _from = from;
    _caret = to;
  }

  void drag_line() {
    _caret = column_at(_pointer.x);
    _from = *_line_pressed == _caret ? std::nullopt : std::optional(*_line_pressed);
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
    _line_left = typed;
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
            .extent = {Caret::width, static_cast<std::uint32_t>(cell.y)},
            .role = VP_VIEW::role("accent")};
  }

  // The keys a search takes: enter and control+f find its words again, and escape ends
  // it; tab, up and down do nothing, as nothing completes or recalls a search. Whether it
  // took the key; the rest edit the words.
  bool finds(VP::Cook &frame, std::string_view key) {
    if (key == "enter" || (_control && key == "f")) {
      if (_line.find_first_not_of(' ') == std::string::npos)
        return true;
      if (_finder)
        _answer =
            frame.commands().send(std::format("{}: {} {}", _own_view, _finds, _line));
      else
        find_above();
    } else if (key == "escape") {
      if (_finder) {
        _open = false;
        frame.commands().send(std::format("{}: focus {}", _own_view, _host));
      } else {
        _finding = false;
        _words = std::exchange(_line, std::exchange(_set_aside, {}));
        _caret = _line.size();
      }
    } else {
      return key == "tab" || key == "up" || key == "down";
    }
    return true;
  }

  // The words in the rows above, from the match found last up to the oldest row, then on
  // from the newest down to it, as a shell's search goes back in time: the match is
  // selected, and scrolled to.
  void find_above() {
    const std::size_t rows = _scrollback.size();
    if (rows == 0)
      return;
    if (_found && _found->row < _dropped) // dropped with the oldest rows since
      _found.reset();
    const std::size_t start = _found ? _found->row - _dropped : rows;
    for (std::size_t step = 0; step <= rows; ++step) {
      const std::size_t index = (start + 2 * rows - step - (_found ? 0 : 1)) % rows;
      const std::string &text = _scrollback[index].text;
      const bool first = step == 0 && _found;
      const std::size_t at =
          first ? (_found->column == 0 ? std::string::npos
                                       : text.rfind(_line, _found->column - 1))
                : text.rfind(_line);
      if (at == std::string::npos || (step == rows && _found && at < _found->column))
        continue;
      _found = Mark{_dropped + index, at};
      _anchor = *_found;
      _end = {_found->row, at + _line.size()};
      _scroll.show(index);
      _columns.show(at);
      return;
    }
  }

  // Control+f in its host opens it, taking the focus, as a press on its row does, which
  // puts the caret there too; open, it takes its own keys, and its row leaves the host
  // the rest of its area.
  void find_bar(VP::Cook &frame) {
    for (const VP::Event &event : frame.input().events())
      point_bar(frame, event);
    if (_typed->focus != _host)
      _host_control = false;
    for (const VP::Event &event : _typed->to(_host))
      if (event.kind == VP::Event::Kind::key && event.name == "control")
        _host_control = event.down;
      else if (event.kind == VP::Event::Kind::key && event.down && event.name == "f" &&
               _host_control) {
        _open = true;
        _answer.clear();
        frame.commands().send(std::format("{}: focus {}", _own_view, _name));
      }
    if (!focused())
      _control = false;
    if (_open)
      for (const VP::Event &event : _typed->to(_name)) {
        if (event.kind == VP::Event::Kind::text)
          _answer.clear();
        edit(frame, event);
      }
    lay_out_bar(frame);
  }

  // As a press on the terminal's line does, a press on the bar puts the caret, and a drag
  // selects; the focus comes with it, as the host keeps it while the bar is pressed.
  void point_bar(VP::Cook &frame, const VP::Event &event) {
    if (event.kind == VP::Event::Kind::pointer) {
      _pointer = event.at;
      if (_line_pressed)
        drag_line();
    } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
      _line_pressed.reset();
      if (!event.down || !VP_VIEW::contains(_line_row, _pointer))
        return;
      press_line(event.presses);
      if (!focused())
        frame.commands().send(std::format("{}: focus {}", _own_view, _name));
    }
  }

  // Open, a row at the top of its area, ruled off below: the prompt and the words, the
  // caret in focus, and what the last find answered, muted, after them; closed, none.
  void lay_out_bar(VP::Cook &frame) {
    const glm::ivec2 cell(_font->cell);
    const std::int32_t height =
        _open && cell.y > 0
            ? std::min(cell.y + 2 * margin, static_cast<std::int32_t>(_area->extent.y))
            : 0;
    *_rest = {.offset = _area->offset + glm::ivec2(0, height),
              .extent = _area->extent - glm::uvec2(0, static_cast<std::uint32_t>(height))};
    const auto start = static_cast<std::int32_t>(find_prompt.size());
    const std::int32_t columns =
        cell.x == 0 ? 0 : (static_cast<std::int32_t>(_area->extent.x) - 2 * margin) / cell.x;
    _line_row = {};
    if (height < cell.y + 2 * margin || columns <= start) {
      frame.write(_rects, 0);
      frame.write(_labels, 0);
      frame.write(_characters, 0);
      return;
    }
    _line_row = {.offset = _area->offset,
                 .extent = {_area->extent.x, static_cast<std::uint32_t>(height)}};
    const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
    rects[0] = {.offset = _area->offset,
                .extent = {_area->extent.x, static_cast<std::uint32_t>(height)},
                .role = VP_VIEW::role("panel")};
    rects[1] = {.offset = _area->offset + glm::ivec2(0, height - 1),
                .extent = {_area->extent.x, rule_width},
                .role = VP_VIEW::role("border")};
    std::uint32_t count = 2;
    Labels text{frame.write(_labels), frame.write(_characters)};
    const glm::ivec2 at = _area->offset + glm::ivec2(margin, margin);
    const VP_VIEW::Rect caret =
        lay_out_line(text, find_prompt, at.x, columns - start, cell, at.y);
    if (const auto chosen = line_selected(columns - start, cell, at.y))
      rects[count++] = *chosen;
    // Two blanks past the words, as much of its first line as the row holds.
    if (const auto past = start + static_cast<std::int32_t>(_line.size()) + 2;
        !_answer.empty() && past < columns)
      text.add({.offset = at + glm::ivec2(past * cell.x, 0),
                .extent = glm::uvec2(static_cast<std::uint32_t>((columns - past) * cell.x),
                                     cell.y),
                .role = VP_VIEW::role("muted")},
               lines_of(_answer).front().substr(0, static_cast<std::size_t>(columns - past)));
    const bool typed = !_typed->to(_name).empty();
    if (focused() && _blink.shows(frame.index(), caret.offset, typed))
      rects[count++] = caret;
    frame.write(_rects, count);
    frame.write(_labels, text.label_count);
    frame.write(_characters, text.character_count);
  }

  void command(VP::Call &call) override {
    if (call.is(_help))
      help(call);
    else if (call.is(_complete))
      complete(call);
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
  std::string _view;   // the current view, as last found; empty for the host
  std::string _folder; // that view's, or the graph's place, as the prompt shows it
  const VP_VIEW::Place *_place = nullptr; // a connection's; none for none
  const VP_VIEW::Handed *_handed = nullptr; // a connection's; none for none
  std::uint64_t _taken = 0;                 // the handed batches it took
  std::string _placed;                    // its folder, as the prompt last showed it
  bool _followed = false; // until the first frame finds the current view
  std::vector<std::string> _kept; // the views the log keeps to, as it was told
  bool _prompted = false;
  bool _window = false;
  VP::TerminalPort *_terminal = nullptr; // while a person's keys edit the line there
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
  // Where the selection of the line starts, the caret at its other end; none for none.
  std::optional<std::size_t> _from;
  // Where the line typed shows, which a press reaches, and its first character, across;
  // and where a press on it put the caret, which a drag selects from until let go.
  VP_VIEW::Rect _line_row{};
  std::int32_t _line_left = 0;
  std::optional<std::size_t> _line_pressed;
  bool _shift = false; // held, so control with z makes an edit taken back again
  // The line before each edit, and before each taken back, the newest last; whether the
  // last edit typed, and where it left the caret, so the next typed there joins it; and
  // how many lines were sent, as each starts the edits anew.
  std::vector<Line> _undone;
  std::vector<Line> _redone;
  bool _typing = false;
  std::size_t _typed_to = 0;
  std::size_t _sent = 0;
  Caret _blink;           // the caret's mark, as it blinks
  std::size_t _first = 0; // the first character of the line shown
  std::vector<std::string> _history;
  std::size_t _recalled = 0; // the line recalled; past the history for a new one
  // A search: whether the line is one, the line typed it set aside, the words searched
  // for last, and where they were found last, which the next find goes on from.
  bool _finding = false;
  std::string _set_aside;
  std::string _words;
  std::optional<Mark> _found;
  // As a find bar: the node whose control+f opens it, held as its keys say, the view its
  // lines go to, whether it is open, what the last find answered, and the Rect it leaves
  // the host.
  bool _finder = false;
  std::string _host;
  std::string _finds; // the command its words go to, its words joined by blanks
  bool _host_control = false;
  std::string _own_view;
  bool _open = false;
  std::string _answer;
  VP_VIEW::Rect *_rest = nullptr;
};

} // namespace

VP_OPERATOR(CommandLine);
