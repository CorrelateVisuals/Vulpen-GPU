#include "Text.h"

#include "contracts/Palette.h"
#include "runtime/View.h"
#include "shared/Paths.h"
#include "shared/Words.h"
#include "Sources.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <stdexcept>

namespace {

constexpr std::int32_t margin = 4; // around the text, as the terminal's
constexpr std::size_t row_room = 256; // rows shown at most; a taller area shows these
constexpr std::uint32_t label_room = 16384; // a run of one role a Label, in rows shown
constexpr std::uint32_t character_room = 131072; // the rows' characters together
// The ground, a selection Rect a row shown, the caret and the scroll bars' thumbs.
constexpr std::uint32_t rect_room = row_room + 4;
constexpr std::size_t indent = 2; // Tab types spaces to a multiple of this column
// The edits a file keeps to take back, the oldest going past them, so memory stays
// bounded (A03); and the key that takes one back with control, with shift too again.
constexpr std::size_t undo_room = 1000;
constexpr std::string_view undo_key = "z";
// The tab of an editor holding no file, so it still shows as open, empty, and a strip that
// stacks it with other contents has a tab to show it again by.
constexpr std::string_view empty_tab = "editor";
// The ports a chain of editors joins by: the editors before a text node, and those it
// hands on, itself last.
constexpr std::string_view before_port = "before";
constexpr std::string_view editors_port = "editors";
// How deep source follows the headers a file includes, how many files it reads at most,
// and how large one, so a key press reads a bounded share of the tree.
constexpr std::size_t include_depth = 3;
constexpr std::size_t source_room = 128;
constexpr std::size_t source_largest = 1 << 19;
// UTF-8 (RFC 3629): a byte that only continues a character starts with the bits 10.
constexpr unsigned char continuation_mask = 0xC0;
constexpr unsigned char continuation = 0x80;

bool continues(char byte) {
  return (static_cast<unsigned char>(byte) & continuation_mask) == continuation;
}

// Each line without its break, \n or Windows' \r\n, so joined with the file's breaks they
// give back the text, a last break included: a file changes only where it was edited.
std::vector<std::string> lines_of(std::string_view text) {
  std::vector<std::string> lines;
  for (std::size_t at = 0;;) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    std::string_view line = text.substr(at, end - at);
    if (end < text.size() && line.ends_with('\r'))
      line.remove_suffix(1);
    lines.emplace_back(line);
    if (end == text.size())
      return lines;
    at = end + 1;
  }
}

std::string joined(const std::vector<std::string> &lines, bool crlf) {
  std::string text = lines.front();
  for (const std::string &line : std::span(lines).subspan(1))
    text.append(crlf ? "\r\n" : "\n").append(line);
  return text;
}

// Its folder and name, which tell apart the many view.vlp of a project, and a mark while
// it has changes.
std::string label_of(const Document &document) {
  const std::filesystem::path path(document.path);
  return std::format("{}/{}{}",
                     path.parent_path().filename().string(),
                     path.filename().string(),
                     document.changed ? "*" : "");
}

std::pair<Spot, Spot> selection(const Document &document) {
  return std::minmax(document.caret, document.anchor);
}

// Where the character before a spot starts; the line before's end at a row's start.
Spot before(const Document &document, Spot spot) {
  if (spot.column == 0)
    return spot.row == 0 ? spot : Spot{spot.row - 1, document.lines[spot.row - 1].size()};
  const std::string &line = document.lines[spot.row];
  do
    --spot.column;
  while (spot.column > 0 && continues(line[spot.column]));
  return spot;
}

// Where the character after a spot starts; the next line's start at a row's end.
Spot after(const Document &document, Spot spot) {
  const std::string &line = document.lines[spot.row];
  if (spot.column == line.size())
    return spot.row + 1 == document.lines.size() ? spot : Spot{spot.row + 1, 0};
  do
    ++spot.column;
  while (spot.column < line.size() && continues(line[spot.column]));
  return spot;
}

// The spot nearest a row and a column that the text holds, at a character's start.
Spot at(const Document &document, std::size_t row, std::size_t column) {
  row = std::min(row, document.lines.size() - 1);
  const std::string &line = document.lines[row];
  column = std::min(column, line.size());
  while (column > 0 && column < line.size() && continues(line[column]))
    --column;
  return {row, column};
}

// Takes out the text between two spots, joining their rows, and leaves the caret there.
void erase(Document &document, Spot from, Spot to) {
  document.caret = document.anchor = from;
  if (from == to)
    return;
  std::vector<std::string> &lines = document.lines;
  const std::string rest = lines[to.row].substr(to.column);
  lines[from.row].erase(from.column).append(rest);
  lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(from.row) + 1,
              lines.begin() + static_cast<std::ptrdiff_t>(to.row) + 1);
  document.changed = document.restyle = true;
}

// The rows an edit at the caret or of the selection may change, as they are before it:
// the selection's and the caret's, and a row either side, for a break it takes out.
Change before_edit(const Document &document) {
  const auto [from, to] = selection(document);
  const std::size_t low = std::min(from.row, document.caret.row);
  const std::size_t first = low == 0 ? 0 : low - 1;
  const std::size_t last =
      std::min(std::max(to.row, document.caret.row) + 1, document.lines.size() - 1);
  const auto row = document.lines.begin() + static_cast<std::ptrdiff_t>(first);
  return {.first = first,
          .before = std::vector<std::string>(
              row, row + static_cast<std::ptrdiff_t>(last - first) + 1),
          .caret_before = document.caret,
          .anchor_before = document.anchor};
}

// The edit since before_edit, kept to take back, unless it changed nothing, as a copy;
// rows: as many as the file held before it. Characters typed join those typed just
// before them while the caret stayed where those left it, and no blank starts a word,
// so a word goes back at once; never past the text the file holds, so changed follows.
void keep(Document &document, Change change, std::size_t rows, std::string_view typed) {
  const std::size_t count = change.before.size() + document.lines.size() - rows;
  const auto from = document.lines.begin() + static_cast<std::ptrdiff_t>(change.first);
  if (count == change.before.size() &&
      std::equal(change.before.begin(), change.before.end(), from))
    return;
  change.after.assign(from, from + static_cast<std::ptrdiff_t>(count));
  change.caret_after = document.caret;
  change.anchor_after = document.anchor;
  change.typed = !typed.empty();
  std::vector<Change> &undone = document.undone;
  if (change.typed && !undone.empty() && undone.back().typed && typed.front() != ' ' &&
      undone.back().caret_after == change.caret_before &&
      undone.back().first == change.first &&
      undone.back().after.size() == change.before.size() &&
      document.saved != undone.size()) {
    undone.back().after = std::move(change.after);
    undone.back().caret_after = change.caret_after;
    undone.back().anchor_after = change.anchor_after;
    return;
  }
  if (document.saved && *document.saved > undone.size())
    document.saved.reset();
  undone.push_back(std::move(change));
  document.redone.clear();
  if (undone.size() > undo_room) {
    undone.erase(undone.begin());
    document.saved = document.saved && *document.saved > 0
                         ? std::optional<std::size_t>(*document.saved - 1)
                         : std::nullopt;
  }
}

// The last edit taken back, or with again the last taken back made again, the caret and
// the selection where they were then.
void undo(Document &document, bool again) {
  std::vector<Change> &from = again ? document.redone : document.undone;
  std::vector<Change> &to = again ? document.undone : document.redone;
  if (from.empty())
    return;
  Change &change = from.back();
  const auto at = static_cast<std::ptrdiff_t>(change.first);
  const std::vector<std::string> &now = again ? change.before : change.after;
  const std::vector<std::string> &then = again ? change.after : change.before;
  const auto rows = document.lines.begin() + at;
  document.lines.erase(rows, rows + static_cast<std::ptrdiff_t>(now.size()));
  document.lines.insert(document.lines.begin() + at, then.begin(), then.end());
  document.caret = again ? change.caret_after : change.caret_before;
  document.anchor = again ? change.anchor_after : change.anchor_before;
  to.push_back(std::move(change));
  from.pop_back();
  document.changed = document.saved != document.undone.size();
  document.restyle = true;
}

// The selection as text, a break between its rows, as a copy takes it.
std::string selected(const Document &document) {
  const auto [from, to] = selection(document);
  if (from.row == to.row)
    return document.lines[from.row].substr(from.column, to.column - from.column);
  std::string text = document.lines[from.row].substr(from.column);
  for (std::size_t row = from.row + 1; row < to.row; ++row)
    text.append(1, '\n').append(document.lines[row]);
  return text.append(1, '\n').append(document.lines[to.row], 0, to.column);
}

// What follows the caret goes down a row, behind the first spaces of the row it was on.
void break_row(Document &document, std::size_t spaces) {
  const Spot caret = document.caret;
  std::string &line = document.lines[caret.row];
  std::string below = line.substr(0, spaces) + line.substr(caret.column);
  line.erase(caret.column);
  document.lines.insert(
      document.lines.begin() + static_cast<std::ptrdiff_t>(caret.row) + 1,
      std::move(below));
  document.caret = document.anchor = {caret.row + 1, spaces};
  document.changed = document.restyle = true;
}

// Text typed or pasted at the caret, in place of the selection; each break in it, as a
// file from another system ends a line too, starts a row.
void insert(Document &document, std::string_view text) {
  const auto [from, to] = selection(document);
  erase(document, from, to);
  for (std::size_t end = text.find('\n');; end = text.find('\n')) {
    const std::string_view line = text.substr(0, end);
    const std::string_view kept = line.ends_with('\r') ? line.substr(0, line.size() - 1)
                                                        : line;
    document.lines[document.caret.row].insert(document.caret.column, kept);
    document.caret.column += kept.size();
    if (end == std::string_view::npos)
      break;
    break_row(document, 0);
    text.remove_prefix(end + 1);
  }
  document.anchor = document.caret;
  document.changed = document.restyle = true;
}

// Enter: what follows the caret goes down a row, behind the spaces the row starts with,
// so a block of code keeps its indent.
void split(Document &document) {
  const auto [from, to] = selection(document);
  erase(document, from, to);
  const std::string &line = document.lines[document.caret.row];
  break_row(document, std::min(line.find_first_not_of(' '), document.caret.column));
}

// A key that changes the text: a break, an indent, or the selection or a character gone;
// with control, backspace takes the word before the caret, or at a row's start its break,
// and delete the word after it, or at a row's end its break.
bool changed_by(Document &document, std::string_view key, bool control) {
  const auto [from, to] = selection(document);
  const Spot caret = document.caret;
  const std::string &row = document.lines[caret.row];
  if (key == "enter")
    split(document);
  else if (key == "tab")
    insert(document, std::string(indent - caret.column % indent, ' '));
  else if ((key == "backspace" || key == "delete") && from != to)
    erase(document, from, to);
  else if (key == "backspace" && control && caret.column > 0)
    erase(document, {caret.row, word_before(row, caret.column)}, caret);
  else if (key == "delete" && control && caret.column < row.size())
    erase(document, caret, {caret.row, word_after(row, caret.column)});
  else if (key == "backspace")
    erase(document, before(document, caret), caret);
  else if (key == "delete")
    erase(document, caret, after(document, caret));
  else
    return false;
  return true;
}

glm::ivec2 cell_at(const Grid &grid, std::size_t row, std::size_t column) {
  return grid.origin + glm::ivec2(static_cast<std::int32_t>(column) * grid.cell.x,
                                  static_cast<std::int32_t>(row) * grid.cell.y);
}

// So many cells side by side.
glm::uvec2 extent_of(const Grid &grid, std::size_t columns) {
  return glm::uvec2(static_cast<std::uint32_t>(columns), 1) * glm::uvec2(grid.cell);
}

} // namespace

namespace {

// The file param names the file to show, as open sets it: a change opens it at the next
// cook or command, where the file port is. The same name again, as after a rebuild,
// changes nothing.
void Text::bind(VP::Bind &node) {
  std::string file;
  if (node.sets("file"))
    file = (std::filesystem::path(node.folder()) / node.param<std::string>("file"))
               .string();
  if (std::exchange(_file, file) != file)
    _opening = !file.empty();
  _name = node.name();
  _view = node.view_name();
  _library = node.folder();
  // The editors before it come through a connection to its before, when one brings them,
  // and it hands them on with itself; the last of the chain, whose editors no text node's
  // before takes, answers the editor's commands for them all.
  const std::span<const VP::Connection> connections = node.view().connections;
  const bool brought = std::ranges::any_of(connections, [&](const VP::Connection &c) {
    return std::ranges::any_of(c.to, [&](const VP::Endpoint &to) {
      return to.node == _name && to.port == before_port;
    });
  });
  _head = std::ranges::none_of(connections, [&](const VP::Connection &c) {
    return c.from.node == _name && c.from.port == editors_port &&
           std::ranges::any_of(c.to, [](const VP::Endpoint &to) {
             return to.port == before_port;
           });
  });
  _before = brought ? &node.input<VP_VIEW::Editors>(before_port) : nullptr;
  _editors = &node.output<VP_VIEW::Editors>(editors_port);
  *_editors = _before ? *_before : VP_VIEW::Editors{};
  _editors->push_back(this);
  if (_head) {
    _write = node.command("write", "saves the file the editor in focus shows");
    _close = node.command("close [<file>]",
                          "closes a file an editor holds, or with none the one the "
                          "editor in focus shows, unless it has changes");
    _close_force = node.command("close force [<file>]",
                                "closes a file an editor holds, or with none the one the "
                                "editor in focus shows, dropping its changes");
    _close_left = node.command("close left [<file>]",
                               "closes the files left of a file's tab, or of the one the "
                               "editor in focus shows, but those with changes");
    _close_right = node.command("close right [<file>]",
                                "closes the files right of a file's tab, or of the one "
                                "the editor in focus shows, but those with changes");
    _close_all = node.command("close all [<file>]",
                              "closes every file of the editor holding a file, or of the "
                              "editor in focus, but those with changes");
    _find = node.command("find <value>...",
                         "selects the next place the file the editor in focus shows holds "
                         "the words, joined by single blanks, after the caret, and from "
                         "its start past its end");
    _move = node.command("tab move <file> <file>",
                         "puts the first file's tab where the second's is, in the editor "
                         "holding both, as a drag of one onto the other does");
    _jump = node.command(
        "jump [<value>]",
        "shows where the name at the caret of the editor in focus comes from, or the "
        "name given: the file a path there names, else the next place the file, those it "
        "includes or those beside it declare or define the name, as F12 does");
  }
  _area = &node.input<VP_VIEW::Rect>("area");
  _font = &node.input<VP_VIEW::Font>("font");
  _typed = &node.input<VP_VIEW::Typed>("typed");
  _tabs = &node.output<VP_VIEW::Items>("tabs");
  _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
  _labels = node.upload<VP_VIEW::Label>("labels", label_room);
  _characters = node.upload<VP_VIEW::Character>("characters", character_room);
  name_tabs();
}

// The keys first: the keys part hands on only those that came before this frame's first
// press, so a press moves the caret after them, as they came. A key let go while another
// part had the focus never came, so out of focus shift and control count as up.
void Text::cook(VP::Cook &frame) {
  if (std::exchange(_opening, false)) {
    try {
      route(frame.files());
    } catch (const std::runtime_error &failure) {
      frame.log(VP::Level::error, failure.what());
    }
  }
  if (_head && holder_named(_typed->focus))
    _last = _typed->focus;
  const Grid grid = this->grid();
  if (!focused())
    _shift = _control = false;
  for (const VP::Event &event : _typed->to(_name))
    if (!escape(frame, event, grid))
      edit(frame.input(), event, grid);
  if (!_documents.empty()) {
    Document &document = _documents[_shown];
    if (std::exchange(document.restyle, false))
      measure(document);
    document.scroll.fit(document.lines.size(), grid.rows, document.lines.size() - 1);
    // A column past the widest row, where the caret goes at its end.
    const std::size_t columns = document.widest + 1;
    document.columns.fit(
        columns, grid.columns, columns - std::min(columns, grid.columns));
  }
  point(frame, grid);
  if (_dragging && !_documents.empty() && grid.rows > 0)
    drift(_documents[_shown], grid);
  lay_out(frame, grid);
  if (!_unsaved.empty())
    frame.hold(_unsaved);
}

// The text keeps clear of the scroll bars, and of a strip as wide as a bar at its left,
// so it sits as far in from either side.
Grid Text::grid() const {
  const glm::ivec2 cell(_font->cell);
  const glm::ivec2 room =
      glm::ivec2(_area->extent) - 2 * margin - glm::ivec2(2, 1) * Scroll::width;
  if (cell.x <= 0 || cell.y <= 0 || room.x < cell.x || room.y < cell.y)
    return {};
  return {.origin = _area->offset + margin + glm::ivec2(Scroll::width, 0),
          .cell = cell,
          .rows = std::min(static_cast<std::size_t>(room.y / cell.y), row_room),
          .columns = static_cast<std::size_t>(room.x / cell.x)};
}

bool Text::focused() const {
  return _typed->focus == _name;
}

// In the order the events came, so a press is tested where the pointer was then. A drag
// moves the caret and leaves the selection's start where it was pressed; a press on a
// scroll bar only scrolls. The wheel scrolls across as it turns sideways, or with shift
// held, which the window says whatever part has the focus.
void Text::point(VP::Cook &frame, const Grid &grid) {
  Document *const document =
      _documents.empty() || grid.rows == 0 ? nullptr : &_documents[_shown];
  for (const VP::Event &event : frame.input().events()) {
    const bool inside = VP_VIEW::contains(*_area, _pointer);
    if (event.kind == VP::Event::Kind::pointer) {
      _pointer = event.at;
      if (document) {
        document->scroll.drag(_pointer, document->scroll.track(*_area));
        document->columns.drag(_pointer, document->columns.track(*_area));
      }
      if (_dragging && document) {
        document->caret = spot_at(*document, grid);
        _follow = true;
      }
    } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
      if (document && !event.down) {
        document->scroll.release();
        document->columns.release();
      }
      if (event.down && inside && document &&
          (document->scroll.press(_pointer, document->scroll.track(*_area)) ||
           document->columns.press(_pointer, document->columns.track(*_area))))
        continue;
      _dragging = event.down && inside && document != nullptr;
      if (event.down && inside)
        press(frame, grid, event.presses);
    } else if (event.kind == VP::Event::Kind::wheel && inside && document) {
      const glm::vec2 turn = _sideways ? glm::vec2(event.turn.y, 0.0f) : event.turn;
      document->scroll.wheel(turn.y);
      document->columns.wheel(turn.x);
    } else if (event.kind == VP::Event::Kind::key && event.name == "shift") {
      _sideways = event.down;
    } else if (event.kind == VP::Event::Kind::focus && !event.down) {
      _sideways = false; // let go while the window had no focus, which no event says
    }
  }
}

// A press gives it the focus, and puts the caret where it was. The second in a row
// selects the word pressed, and any more the row with its break, as a cut then takes the
// row whole; a drag leaves either as it is.
void Text::press(VP::Cook &frame, const Grid &grid, std::uint32_t presses) {
  if (!focused())
    frame.commands().send(std::format("focus {}", _name));
  if (!_dragging)
    return;
  Document &document = _documents[_shown];
  document.caret = document.anchor = spot_at(document, grid);
  _follow = true;
  if (presses < 2)
    return;
  _dragging = false;
  const Spot pressed = spot_at(document, grid, true);
  const std::string &line = document.lines[pressed.row];
  if (presses == 2) {
    const auto [from, to] = word_at(line, pressed.column);
    document.anchor = {pressed.row, from};
    document.caret = {pressed.row, to};
  } else {
    document.anchor = {pressed.row, 0};
    document.caret = pressed.row + 1 < document.lines.size()
                         ? Spot{pressed.row + 1, 0}
                         : Spot{pressed.row, line.size()};
  }
}

// The spot nearest the pointer among the rows and columns shown: the row it is on, and
// the boundary between characters closest to it, so a press between two puts the caret
// between them; or with character, the start of the character it is on, as a double
// click's word is found from. A pointer past them is at their edge, where a drag's
// selection waits for them to scroll on.
Spot Text::spot_at(const Document &document, const Grid &grid, bool character) const {
  const glm::vec2 cells = (_pointer - glm::vec2(grid.origin)) / glm::vec2(grid.cell);
  const auto row = static_cast<std::size_t>(
      std::clamp(std::floor(cells.y), 0.0f, static_cast<float>(grid.rows - 1)));
  const auto column = static_cast<std::size_t>(
      std::clamp(character ? std::floor(cells.x) : std::round(cells.x),
                 0.0f,
                 static_cast<float>(grid.columns)));
  return at(document, document.scroll.first() + row, document.columns.first() + column);
}

// Each frame a drag selects with the pointer past the rows or the columns shown, they
// scroll on, and the caret with them, so a selection reaches what does not show.
void Text::drift(Document &document, const Grid &grid) {
  const glm::vec2 cells = (_pointer - glm::vec2(grid.origin)) / glm::vec2(grid.cell);
  const glm::vec2 shown(static_cast<float>(grid.columns), static_cast<float>(grid.rows));
  const glm::vec2 past = glm::min(cells, 0.0f) + glm::max(cells - shown, 0.0f);
  if (past == glm::vec2(0.0f))
    return;
  document.scroll.drift(past.y);
  document.columns.drift(past.x);
  document.caret = spot_at(document, grid);
  _follow = true;
}

// Escape lets go of the selection, and with none of the focus, so the next escape reaches
// what takes the keys no part in focus takes. With no room it takes none.
bool Text::escape(VP::Cook &frame, const VP::Event &event, const Grid &grid) {
  if (event.kind != VP::Event::Kind::key || !event.down || event.name != "escape" ||
      grid.rows == 0)
    return false;
  if (Document *const document = _documents.empty() ? nullptr : &_documents[_shown];
      document && document->anchor != document->caret)
    document->anchor = document->caret;
  else
    frame.commands().send("focus");
  return true;
}

// Typed text goes in at the caret, and a key moves the caret, selecting while shift is
// held, changes the text, or with control copies, cuts or pastes, or takes the last edit
// back, with shift too makes it again; any other does nothing here. With no room it
// takes none.
void Text::edit(VP::InputPort &input, const VP::Event &event, const Grid &grid) {
  if (event.kind == VP::Event::Kind::key && event.name == "shift")
    _shift = event.down;
  if (event.kind == VP::Event::Kind::key && event.name == "control")
    _control = event.down;
  if (grid.rows == 0 || _documents.empty() ||
      (event.kind == VP::Event::Kind::key && !event.down))
    return;
  Document &document = _documents[_shown];
  const bool changed = document.changed;
  const std::size_t rows = document.lines.size();
  if (event.kind == VP::Event::Kind::text) {
    Change change = before_edit(document);
    for (const char typed : event.name)
      if (typed >= VP_VIEW::first_printable && typed <= VP_VIEW::last_printable)
        insert(document, std::string_view(&typed, 1));
    keep(document, std::move(change), rows, event.name);
  } else if (_control && event.name == undo_key) {
    undo(document, _shift);
  } else if (const std::optional<Spot> to = moved(document, event.name, grid)) {
    document.caret = *to;
    if (!_shift)
      document.anchor = *to;
  } else if (Change change = before_edit(document);
             (_control && clip(input, document, event.name)) ||
             changed_by(document, event.name, _control)) {
    keep(document, std::move(change), rows, {});
  } else {
    return;
  }
  _follow = true;
  if (document.changed != changed)
    name_tabs();
}

// Control with c copies the selection, with x cuts it and with v pastes in its place;
// false for any other key. With nothing selected, a copy or a cut leaves the clipboard
// as it was, and an empty clipboard pastes nothing.
bool Text::clip(VP::InputPort &input, Document &document, std::string_view key) const {
  const auto [from, to] = selection(document);
  if ((key == "c" || key == "x") && from != to)
    input.copy(selected(document));
  if (key == "x")
    erase(document, from, to);
  else if (const std::string pasted = key == "v" ? input.clipboard() : "";
           !pasted.empty())
    insert(document, pasted);
  return key == "c" || key == "x" || key == "v";
}

// Where a key moves the caret: a character, a row or a page over, or to its row's start
// or end; with control, left and right a word over, as far as control with backspace
// and delete take, or at a row's start or end past its break. Without shift, left and
// right leave a selection at its start or its end.
std::optional<Spot>
Text::moved(const Document &document, std::string_view key, const Grid &grid) const {
  const Spot caret = document.caret;
  const auto [from, to] = selection(document);
  const bool selected = from != to && !_shift;
  const std::size_t rows = key.starts_with("page") ? grid.rows : 1;
  const std::string &row = document.lines[caret.row];
  if (key == "left" && _control && caret.column > 0)
    return Spot{caret.row, word_before(row, caret.column)};
  if (key == "right" && _control && caret.column < row.size())
    return Spot{caret.row, word_after(row, caret.column)};
  if (key == "left")
    return selected ? from : before(document, caret);
  if (key == "right")
    return selected ? to : after(document, caret);
  if (key == "up" || key == "page_up")
    return at(document, caret.row - std::min(caret.row, rows), caret.column);
  if (key == "down" || key == "page_down")
    return at(document, caret.row + rows, caret.column);
  if (key == "home")
    return Spot{caret.row, 0};
  if (key == "end")
    return Spot{caret.row, document.lines[caret.row].size()};
  return std::nullopt;
}

// The file shown, a Label a row, over its ground, the selection and, in focus, the caret.
// With no file or no room it writes nothing, so it costs no draw.
void Text::lay_out(VP::Cook &frame, const Grid &grid) {
  if (_documents.empty() || grid.rows == 0) {
    frame.write(_rects, 0);
    frame.write(_labels, 0);
    frame.write(_characters, 0);
    return;
  }
  Document &document = _documents[_shown];
  follow(document);
  const std::size_t top = document.scroll.first();
  const std::size_t left = document.columns.first();
  const std::size_t rows = std::min(grid.rows, document.lines.size() - top);
  const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
  rects[0] = {.offset = _area->offset,
              .extent = _area->extent,
              .role = VP_VIEW::role("background")};
  std::size_t count = 1 + select(document, grid, rows, rects.subspan(1));
  const Spot caret = document.caret;
  if (focused() && caret.row >= top && caret.row < top + rows && caret.column >= left &&
      caret.column < left + grid.columns) {
    const glm::ivec2 at = cell_at(grid, caret.row - top, caret.column - left);
    if (_blink.shows(frame.index(), at, !_typed->to(_name).empty()))
      rects[count++] = {.offset = at,
                        .extent = {Caret::width, static_cast<std::uint32_t>(grid.cell.y)},
                        .role = VP_VIEW::role("accent")};
  }
  for (const Scroll *scroll : {&document.scroll, &document.columns})
    if (const std::optional<VP_VIEW::Rect> thumb = scroll->thumb(scroll->track(*_area)))
      rects[count++] = *thumb;
  frame.write(_rects, count);
  write_lines(frame, document, grid, rows);
}

// The first row and column shown, scrolled so the caret shows once it moved; the wheel
// alone may leave it out of view.
void Text::follow(Document &document) {
  if (!std::exchange(_follow, false))
    return;
  document.scroll.show(document.caret.row);
  document.columns.show(document.caret.column);
}

// A Rect a row shown, behind the part of it selected, a row's break as one cell more.
std::size_t Text::select(const Document &document,
                         const Grid &grid,
                         std::size_t rows,
                         std::span<VP_VIEW::Rect> rects) const {
  const auto [from, to] = selection(document);
  const std::size_t left = document.columns.first();
  std::size_t count = 0;
  for (std::size_t shown = 0; shown < rows && from != to; ++shown) {
    const std::size_t row = document.scroll.first() + shown;
    if (row < from.row || row > to.row)
      continue;
    const std::size_t first = std::max(row == from.row ? from.column : 0, left);
    const std::size_t end =
        std::min(row == to.row ? to.column : document.lines[row].size() + 1,
                 left + grid.columns);
    if (first < end)
      rects[count++] = {.offset = cell_at(grid, shown, first - left),
                        .extent = extent_of(grid, end - first),
                        .role = VP_VIEW::role("border")};
  }
  return count;
}

// What each row starts inside, as a block comment, and the widest row, from the file's
// top, once an edit came, so a frame scans only the rows it shows.
void Text::measure(Document &document) {
  document.inside.assign(document.lines.size(), Open::none);
  document.widest = 0;
  Open open = Open::none;
  for (std::size_t row = 0; row < document.lines.size(); ++row) {
    document.inside[row] = open;
    document.widest = std::max(document.widest, document.lines[row].size());
    open = scan(document.lines[row],
                document.syntax,
                open,
                [](std::size_t, std::size_t, std::uint32_t) {});
  }
}

// A Label a run of one role in a row shown, holding the bytes of it that fit from the
// first column shown; what the room lacks is cut.
void Text::write_lines(VP::Cook &frame,
                       const Document &document,
                       const Grid &grid,
                       std::size_t rows) {
  Labels written{frame.write(_labels), frame.write(_characters)};
  const std::size_t left = document.columns.first();
  const std::size_t room = written.characters.size();
  for (std::size_t shown = 0; shown < rows; ++shown) {
    const std::size_t row = document.scroll.first() + shown;
    const std::string_view line = document.lines[row];
    const std::size_t right = left + grid.columns;
    const std::uint32_t first = written.label_count; // the row's first Label
    bool cut = false; // a run the room lacked, so no later one joins a Label past it
    scan(line,
         document.syntax,
         document.inside[row],
         [&](std::size_t from, std::size_t to, std::uint32_t role) {
           from = std::max(from, left);
           to = std::min({to, right, from + room - written.character_count});
           if (cut || from >= to)
             return;
           const std::string_view run = line.substr(from, to - from);
           // A run in the role of the one before it on the row joins its Label.
           if (written.label_count > first &&
               written.labels[written.label_count - 1].role == role) {
             written.append(run);
           } else if (!written.add(
                          {.offset = cell_at(grid, shown, from - left), .role = role},
                          run)) {
             cut = true;
             return;
           }
           VP_VIEW::Label &label = written.labels[written.label_count - 1];
           label.extent = extent_of(grid, label.count);
         });
  }
  frame.write(_labels, written.label_count);
  frame.write(_characters, written.character_count); // the used length glyphs draws
}

// Only the last of the chain registers commands, for the editors it was handed and
// itself: write and find act on the editor in focus, or the one in focus last, and close
// on the editor holding the file named, or with none on that one. A file the param names
// opens first, so a script's line after open finds it.
void Text::command(VP::Call &call) {
  if (std::exchange(_opening, false))
    route(call.files());
  if (call.is(_write)) {
    target().write(call);
  } else if (call.is(_close) || call.is(_close_force) || call.is(_close_left) ||
             call.is(_close_right) || call.is(_close_all)) {
    const std::string_view named =
        call.arguments().empty() ? std::string_view() : call.arguments().front();
    VP_VIEW::Editor *const editor = named.empty() ? &target() : holder(named);
    if (!editor)
      throw std::runtime_error(std::format("{} is not open in an editor", named));
    if (call.is(_close_left) || call.is(_close_right) || call.is(_close_all))
      editor->close_beside(call,
                           named,
                           call.is(_close_left)    ? VP_VIEW::Beside::left
                           : call.is(_close_right) ? VP_VIEW::Beside::right
                                                   : VP_VIEW::Beside::all);
    else
      editor->close(call, named, call.is(_close_force));
    // The param names it no longer, so open names it again as a change.
    if (!_file.empty() && !holder(_file)) {
      _file.clear();
      call.commands().send(std::format("{}: param unset {} file", _view, _name));
    }
  } else if (call.is(_find)) {
    target().find(call);
  } else if (call.is(_jump)) {
    jump(call);
  } else if (call.is(_move)) {
    const std::span<const std::string_view> files = call.arguments();
    VP_VIEW::Editor *const editor = holder(files[0]);
    if (!editor || !editor->holds(files[1]))
      throw std::runtime_error(std::format("{} and {} are not open in one editor; a tab "
                                           "moves among its editor's",
                                           files[0],
                                           files[1]));
    editor->move(files[0], files[1]);
  }
}

// The file shown stays shown, wherever it goes.
void Text::move(std::string_view path, std::string_view to) {
  const auto at = [&](std::string_view named) {
    return static_cast<std::size_t>(
        std::ranges::find(_documents, named, &Document::path) - _documents.begin());
  };
  const std::size_t from = at(path);
  const std::size_t place = at(to);
  const std::string shown = _documents[_shown].path;
  if (from < place)
    std::rotate(_documents.begin() + static_cast<std::ptrdiff_t>(from),
                _documents.begin() + static_cast<std::ptrdiff_t>(from) + 1,
                _documents.begin() + static_cast<std::ptrdiff_t>(place) + 1);
  else
    std::rotate(_documents.begin() + static_cast<std::ptrdiff_t>(place),
                _documents.begin() + static_cast<std::ptrdiff_t>(from),
                _documents.begin() + static_cast<std::ptrdiff_t>(from) + 1);
  _shown = at(shown);
  name_tabs();
}

const std::string &Text::name() const {
  return _name;
}

bool Text::holds(std::string_view path) const {
  return std::ranges::find(_documents, path, &Document::path) != _documents.end();
}

std::size_t Text::count() const {
  return _documents.size();
}

std::string_view Text::shown() const {
  return _documents.empty() ? std::string_view() : _documents[_shown].path;
}

bool Text::changed() const {
  return !_documents.empty() && _documents[_shown].changed;
}

void Text::show(VP::FilePort &files, std::string_view path) {
  open(files, path);
}

std::span<const std::string> Text::rows() const {
  return _documents.empty() ? std::span<const std::string>() : _documents[_shown].lines;
}

std::pair<std::size_t, std::size_t> Text::caret() const {
  if (_documents.empty())
    return {};
  return {_documents[_shown].caret.row, _documents[_shown].caret.column};
}

void Text::reveal(VP::FilePort &files,
                  std::string_view path,
                  std::size_t row,
                  std::size_t column,
                  std::size_t length) {
  open(files, path);
  Document &document = _documents[_shown];
  document.anchor = at(document, row, column);
  document.caret = at(document, row, column + length);
}

// What a word given, or the caret of the editor in focus, or in focus last, stands in: a
// path, as an include's or a link's, the file it names, from the file's folder or one
// holding it, as an include reaches, else from this part's folder or one holding it, as
// the engine's headers are reached from a project, or a folder, which the graph shows; a
// manifest's
// recipe, its manifest; else the name's next source: the places a file declares or
// defines it, the file shown first, as typed so far, then those it includes, theirs in
// turn, include_depth deep, then those beside it, each once. The one after the source the
// caret stands at shows, round to the first, in the editor holding its file, or in that
// one, which takes the focus; while the caret stands at one the jump before found, it
// goes on among those, so jump again steps through the sources found from where it began.
void Text::jump(VP::Call &call) {
  VP_VIEW::Editor &editor = target();
  const std::string file(editor.shown());
  if (file.empty())
    throw std::runtime_error("no file is open");
  const std::span<const std::string> rows = editor.rows();
  const auto [row, column] = editor.caret();
  const std::string_view line = rows[row];
  const std::filesystem::path folder = std::filesystem::path(file).parent_path();
  const Syntax syntax = syntax_of(file);
  std::map<std::string, std::vector<std::string>> names;
  Listed listed(call.files(), names);
  std::string name;
  if (!call.arguments().empty()) {
    name = call.arguments().front();
  } else if (std::string path = named_path(listed, folder, line, column, syntax);
             !path.empty() ||
             !(path = named_path(listed, _library, line, column, syntax)).empty()) {
    const std::filesystem::path named(path);
    const std::vector<std::string> &beside = listed.names(named.parent_path().string());
    if (std::ranges::find(beside, named.filename().string() + '/') != beside.end())
      call.commands().send(std::format("open {}/", path));
    else
      show_source(call, editor, {.file = std::move(path)});
    return;
  } else if (const auto found = name_at(line, column)) {
    name = syntax == Syntax::keyed && !path_at(line, column).empty()
               ? std::string(path_at(line, column))
               : std::string(line.substr(found->first, found->second - found->first));
  } else {
    throw std::runtime_error("the caret stands in no name and no path");
  }
  const auto at = [&](const Source &source) {
    return source.file == file && source.row == row && source.column <= column &&
           column <= source.column + source.length;
  };
  if (name != _jumped || std::ranges::none_of(_found_last, at)) {
    _found_last = sources(call.files(), listed, file, rows, name);
    _jumped = name;
  }
  const std::vector<Source> &found = _found_last;
  if (found.empty())
    throw std::runtime_error(std::format(
        "no place the file, those it includes or those beside it declare {}", name));
  const auto here = std::ranges::find_if(found, at);
  if (here != found.end() && found.size() == 1)
    throw std::runtime_error(std::format("{} is declared here alone", name));
  const bool round = here == found.end() || here + 1 == found.end();
  show_source(call, editor, round ? found.front() : *(here + 1));
}

// The places the files declare or define the name, in the order jump looks in them.
std::vector<Source> Text::sources(VP::FilePort &files,
                                  Listed &listed,
                                  const std::string &file,
                                  std::span<const std::string> rows,
                                  std::string_view name) const {
  std::vector<std::string> looked{file};
  std::vector<std::vector<std::string>> read{
      std::vector<std::string>(rows.begin(), rows.end())};
  const auto add = [&](const std::string &path) {
    if (path.empty() || looked.size() >= source_room ||
        std::ranges::find(looked, path) != looked.end())
      return;
    std::string text;
    try {
      text = files.read(path);
    } catch (const std::runtime_error &) {
      return; // a folder, or gone
    }
    if (text.size() > source_largest)
      return;
    looked.push_back(path);
    read.push_back(lines_of(text));
  };
  for (std::size_t from = 0, depth = 0; depth < include_depth; ++depth) {
    const std::size_t to = looked.size();
    for (std::size_t at = from; at < to; ++at)
      for (const std::string &line : read[at])
        if (const std::string_view header = included(line); !header.empty()) {
          const std::string beside = resolved(
              listed, std::filesystem::path(looked[at]).parent_path(), header, true);
          add(beside.empty() ? resolved(listed, _library, header, true) : beside);
        }
    from = to;
  }
  const std::filesystem::path folder = std::filesystem::path(file).parent_path();
  for (const std::string &beside : listed.names(folder.string()))
    if (!beside.ends_with('/') && syntax_of(beside) == syntax_of(file))
      add((folder / beside).generic_string());
  std::vector<Source> found;
  for (std::size_t at = 0; at < looked.size(); ++at)
    for (std::size_t line = 0; line < read[at].size(); ++line)
      if (const auto declared = declares(read[at][line], name, syntax_of(looked[at])))
        found.push_back({.file = looked[at],
                         .row = line,
                         .column = declared->first,
                         .length = declared->second});
  return found;
}

// In the editor holding its file, or in the one jump acted on, which takes the focus.
void Text::show_source(VP::Call &call, VP_VIEW::Editor &editor, const Source &source) {
  VP_VIEW::Editor *const holding = holder(source.file);
  VP_VIEW::Editor &showing = holding ? *holding : editor;
  showing.reveal(call.files(), source.file, source.row, source.column, source.length);
  // The file param names it, as open does, so the log keeps what shows, and open names
  // the file it named before as a change.
  call.commands().send(
      std::format("{}: param set {} file {}", _view, _name, source.file));
  if (showing.name() != _typed->focus)
    call.commands().send(std::format("{}: focus {}", _view, showing.name()));
  call.reply(std::format("{}:{}",
                         std::filesystem::path(source.file)
                             .lexically_proximate(std::filesystem::current_path())
                             .generic_string(),
                         source.row + 1));
}

// The editor in focus, or the one in focus last, or with neither this one.
VP_VIEW::Editor &Text::target() {
  for (const std::string &wanted : {_typed->focus, _last})
    if (VP_VIEW::Editor *const editor = holder_named(wanted))
      return *editor;
  return *this;
}

// The editor holding the file; none for none.
VP_VIEW::Editor *Text::holder(std::string_view path) const {
  const auto found = std::ranges::find_if(
      *_editors, [&](const VP_VIEW::Editor *editor) { return editor->holds(path); });
  return found == _editors->end() ? nullptr : *found;
}

// The editor of that name; none for none.
VP_VIEW::Editor *Text::holder_named(std::string_view name) const {
  const auto found = std::ranges::find_if(
      *_editors, [&](const VP_VIEW::Editor *editor) { return editor->name() == name; });
  return found == _editors->end() ? nullptr : *found;
}

// The file its param names: the last of the chain shows it in the editor holding it, or
// else in the one in focus, or in focus last, as a file is open in one editor at a time;
// any other text node in itself.
void Text::route(VP::FilePort &files) {
  if (!_head) {
    open(files, _file);
    return;
  }
  VP_VIEW::Editor *const holding = holder(_file);
  (holding ? *holding : target()).show(files, _file);
}

// Read once, when it opens, so what was typed is never read over.
void Text::open(VP::FilePort &files, std::string_view path) {
  const auto found = std::ranges::find(_documents, path, &Document::path);
  const auto shown = static_cast<std::size_t>(found - _documents.begin());
  if (found == _documents.end()) {
    const std::string text = files.read(path);
    _documents.push_back({.path = std::string(path),
                          .lines = lines_of(text),
                          .crlf = text.find("\r\n") != std::string::npos,
                          .syntax = syntax_of(path)});
  }
  _shown = shown;
  _dragging = false;
  _follow = true;
  name_tabs();
}

void Text::write(VP::Call &call) {
  if (_documents.empty())
    throw std::runtime_error("no file is open");
  Document &document = _documents[_shown];
  call.files().save(document.path, joined(document.lines, document.crlf));
  document.changed = false;
  document.saved = document.undone.size();
  name_tabs();
  call.reply(std::format("wrote {}", document.path));
}

// The file named, as a tab's close mark names it, or the one shown; one with changes only
// by force, so nothing typed goes unasked. The file shown keeps showing, and when it is
// the one closed, the one after it, or else the one before.
void Text::close(VP::Call &call, std::string_view path, bool force) {
  if (_documents.empty())
    throw std::runtime_error("no file is open");
  std::size_t at = _shown;
  if (!path.empty()) {
    const auto found = std::ranges::find(_documents, path, &Document::path);
    if (found == _documents.end())
      throw std::runtime_error(std::format("{} is not open in the editor", path));
    at = static_cast<std::size_t>(found - _documents.begin());
  }
  if (_documents[at].changed && !force)
    throw std::runtime_error(
        std::format("{} has changes: {}write saves them, and close force drops them",
                    label_of(_documents[at]),
                    at == _shown ? "" : "a press on its tab, then "));
  // The param names the file shown no longer, so open names it again as a change.
  if (_documents[at].path == _file) {
    _file.clear();
    call.commands().send(std::format("{}: param unset {} file", _view, _name));
  }
  _documents.erase(_documents.begin() + static_cast<std::ptrdiff_t>(at));
  if (at < _shown)
    --_shown;
  else
    _shown = std::min(_shown, _documents.empty() ? 0 : _documents.size() - 1);
  _dragging = false;
  _follow = true;
  name_tabs();
}

// Of the files beside the one named, or the one shown, those with changes stay, named in
// what it answers, so nothing typed goes unasked; the file shown stays shown, or gives
// way to the one named, or with all of them gone, to the first that stays. A file the
// param names that goes unsets it, as close does.
void Text::close_beside(VP::Call &call, std::string_view path, VP_VIEW::Beside beside) {
  if (_documents.empty())
    throw std::runtime_error("no file is open");
  const auto named = path.empty()
                         ? _documents.begin() + static_cast<std::ptrdiff_t>(_shown)
                         : std::ranges::find(_documents, path, &Document::path);
  if (named == _documents.end())
    throw std::runtime_error(std::format("{} is not open in the editor", path));
  const auto at = static_cast<std::size_t>(named - _documents.begin());
  const std::string named_path = named->path;
  const std::string shown = _documents[_shown].path;
  std::vector<Document> staying;
  std::string kept;
  for (std::size_t index = 0; index < _documents.size(); ++index) {
    Document &document = _documents[index];
    const bool beyond = beside == VP_VIEW::Beside::all ||
                        (beside == VP_VIEW::Beside::left ? index < at : index > at);
    if (!beyond || document.changed) {
      if (beyond)
        kept.append(kept.empty() ? "" : ", ").append(label_of(document));
      staying.push_back(std::move(document));
    } else if (document.path == _file) {
      _file.clear();
      call.commands().send(std::format("{}: param unset {} file", _view, _name));
    }
  }
  _documents = std::move(staying);
  auto still = std::ranges::find(_documents, shown, &Document::path);
  if (still == _documents.end())
    still = std::ranges::find(_documents, named_path, &Document::path);
  _shown = still == _documents.end() ? 0
                                     : static_cast<std::size_t>(still - _documents.begin());
  _dragging = false;
  _follow = true;
  name_tabs();
  if (!kept.empty())
    call.reply(std::format("kept {}, as they have changes", kept));
}

// From the selection's end to the file's end, then from its start, so a find repeated
// steps through every place in turn.
void Text::find(VP::Call &call) {
  if (_documents.empty())
    throw std::runtime_error("no file is open");
  std::string wanted;
  for (const std::string_view word : call.arguments())
    wanted.append(wanted.empty() ? "" : " ").append(word);
  Document &document = _documents[_shown];
  const Spot from = selection(document).second;
  const std::size_t rows = document.lines.size();
  for (std::size_t step = 0; step <= rows; ++step) {
    const std::size_t row = (from.row + step) % rows;
    const std::size_t found =
        document.lines[row].find(wanted, step == 0 ? from.column : 0);
    if (found != std::string::npos && (step < rows || found < from.column)) {
      document.anchor = {row, found};
      document.caret = {row, found + wanted.size()};
      _follow = true;
      return;
    }
  }
  call.reply(std::format("{} holds no {}", label_of(document), wanted));
}

void Text::name_tabs() {
  _tabs->clear();
  if (_documents.empty())
    _tabs->push_back({.label = std::string(empty_tab), .current = true});
  std::string changed;
  std::size_t count = 0;
  for (std::size_t at = 0; at < _documents.size(); ++at) {
    _tabs->push_back({.label = label_of(_documents[at]),
                      .command = std::format("open {}", _documents[at].path),
                      .current = at == _shown,
                      .close = std::format("close {}", _documents[at].path),
                      .move = std::format("tab move {}", _documents[at].path),
                      .menu = std::format("menu tab for {}", _documents[at].path)});
    if (!_documents[at].changed)
      continue;
    changed.append(count++ == 0 ? "" : ", ").append(_tabs->back().label);
  }
  _unsaved = count == 0 ? std::string()
                        : std::format("{} {} changes: write saves the file shown, and "
                                      "close force drops its changes",
                                      changed,
                                      count == 1 ? "has" : "have");
}

} // namespace

VP_OPERATORS(registry) {
  registry.add<Text>("Text");
}
