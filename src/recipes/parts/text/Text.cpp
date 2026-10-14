#include "Text.h"

#include "contracts/Palette.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <stdexcept>

namespace {

constexpr std::int32_t margin = 4; // around the text, as the terminal's
constexpr std::uint32_t caret_width = 2;
constexpr std::size_t row_room = 256; // rows shown at most; a taller area shows these
constexpr std::uint32_t label_room = 16384; // a run of one role a Label, in rows shown
constexpr std::uint32_t character_room = 131072; // the rows' characters together
// The ground, a selection Rect a row shown, and the caret.
constexpr std::uint32_t rect_room = row_room + 2;
constexpr std::size_t indent = 2;  // Tab types spaces to a multiple of this column
constexpr float wheel_rows = 3.0f; // rows a step of the wheel scrolls
constexpr char first_printable = ' '; // the font draws printable ASCII
constexpr char last_printable = '~';
// UTF-8 (RFC 3629): a byte that only continues a character starts with the bits 10.
constexpr unsigned char continuation_mask = 0xC0;
constexpr unsigned char continuation = 0x80;
// The words of C++ and GLSL, which nodes are written in, and the files of each syntax, a
// blank before and after each, so a word is found whole.
constexpr std::string_view keywords =
    " alignas alignof auto break case catch class concept const consteval constexpr"
    " constinit const_cast continue co_await co_return co_yield decltype default delete"
    " do dynamic_cast else enum explicit export extern false final for friend goto if"
    " inline mutable namespace new noexcept nullptr operator override private protected"
    " public reinterpret_cast requires return sizeof static static_assert static_cast"
    " struct switch template this throw true try typedef typeid typename union using"
    " virtual volatile while attribute buffer centroid coherent discard flat highp in"
    " inout invariant layout lowp mediump noperspective out patch precision readonly"
    " restrict sample shared smooth subroutine uniform varying writeonly ";
constexpr std::string_view types = " void bool char int unsigned signed short long float"
                                   " double uint sampler sampler2D texture2D image2D ";
constexpr std::string_view coded = " .cpp .h .hpp .cc .c .glsl .vert .frag .comp ";
constexpr std::string_view keyed = " .vlp .ini ";
constexpr std::string_view exponents = "eEpP"; // a sign after one goes on a number

bool continues(char byte) {
  return (static_cast<unsigned char>(byte) & continuation_mask) == continuation;
}

// Whether the blank-separated words hold the word whole.
bool listed(std::string_view words, std::string_view word) {
  for (std::size_t at = words.find(word); at != std::string_view::npos && !word.empty();
       at = words.find(word, at + 1))
    if (words[at - 1] == ' ' && words[at + word.size()] == ' ')
      return true;
  return false;
}

bool letter(char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

bool digit(char byte) {
  return byte >= '0' && byte <= '9';
}

// A template's file, as Name.cpp.in, by the name it is written for.
Syntax syntax_of(std::string_view path) {
  std::filesystem::path file(path);
  if (file.extension() == ".in")
    file.replace_extension();
  const std::string extension = file.extension().string();
  if (listed(coded, extension))
    return Syntax::code;
  return listed(keyed, extension) ? Syntax::keyed : Syntax::plain;
}

// A type by its word: C++'s and GLSL's own, a name in CamelCase, as types are named here
// (RC01), one ending in _t, and GLSL's vectors and matrices, as uvec2 or mat4x3.
bool typed(std::string_view word) {
  if (listed(types, word) || word.ends_with("_t") ||
      (word.front() >= 'A' && word.front() <= 'Z' &&
       std::ranges::any_of(word, [](char byte) { return byte >= 'a' && byte <= 'z'; })))
    return true;
  if (std::string_view("iubd").find(word.front()) != std::string_view::npos)
    word.remove_prefix(1);
  return (word.starts_with("vec") || word.starts_with("mat")) && word.size() > 3 &&
         word[3] >= '2' && word[3] <= '4';
}

// Past a quote's closing quote, a backslash keeping the next byte in; the row's end
// when it has none.
std::size_t quoted_end(std::string_view row, std::size_t at) {
  for (std::size_t end = at + 1; end < row.size(); ++end)
    if (row[end] == '\\')
      ++end;
    else if (row[end] == row[at])
      return end + 1;
  return row.size();
}

// Past a number's digits, letters, points and separators, as 0x1F, 1.5f, 1'000 or 2e-3.
std::size_t number_end(std::string_view row, std::size_t at) {
  std::size_t end = at + 1;
  while (end < row.size() &&
         (digit(row[end]) || letter(row[end]) || row[end] == '.' || row[end] == '\'' ||
          ((row[end] == '+' || row[end] == '-') &&
           exponents.find(row[end - 1]) != std::string_view::npos)))
    ++end;
  return end;
}

// A word's role: the first of a row in a manifest or an .ini, its key or its section;
// in code, a keyword or a type by the word itself.
std::uint32_t word_role(std::string_view word, Syntax syntax, bool key) {
  if (key || (syntax == Syntax::code && listed(keywords, word)))
    return VP_VIEW::role("keyword");
  return syntax == Syntax::code && typed(word) ? VP_VIEW::role("type")
                                               : VP_VIEW::role("text");
}

// Hands each run of the row in one role to each, as from, to and the role, in order and
// with every byte in one; returns whether a block comment is open at the row's end. In
// code, the comments, a directive, quotes, numbers, and the words by their kind; in a
// manifest or an .ini, the comments, quotes, numbers, and the first word of a row, its
// key or its section.
template <class Each>
bool scan(std::string_view row, Syntax syntax, bool commented, Each each) {
  const std::size_t first = row.find_first_not_of(" \t");
  bool keyed = syntax == Syntax::keyed;
  std::size_t at = 0;
  const auto run = [&](std::size_t to, std::uint32_t role) {
    each(at, to, role);
    at = to;
  };
  while (at < row.size()) {
    const std::string_view rest = row.substr(at);
    if (syntax == Syntax::plain) {
      run(row.size(), VP_VIEW::role("text"));
    } else if (commented || (syntax == Syntax::code && rest.starts_with("/*"))) {
      const std::size_t end = row.find("*/", commented ? at : at + 2);
      commented = end == std::string_view::npos;
      run(commented ? row.size() : end + 2, VP_VIEW::role("comment"));
    } else if (rest.starts_with(syntax == Syntax::code ? "//" : "#")) {
      run(row.size(), VP_VIEW::role("comment"));
    } else if (syntax == Syntax::code && rest.front() == '#' && at == first) {
      run(row.size(), VP_VIEW::role("directive"));
    } else if (rest.front() == '"' || rest.front() == '\'') {
      run(quoted_end(row, at), VP_VIEW::role("string"));
    } else if (digit(rest.front()) ||
               (rest.size() > 1 && rest.front() == '.' && digit(rest[1]))) {
      run(number_end(row, at), VP_VIEW::role("number"));
    } else if (letter(rest.front())) {
      std::size_t end = at + 1;
      while (end < row.size() && (letter(row[end]) || digit(row[end])))
        ++end;
      run(end, word_role(row.substr(at, end - at), syntax, std::exchange(keyed, false)));
    } else {
      run(at + 1, VP_VIEW::role("text"));
    }
  }
  return commented;
}

// Each line without its break, so joined with breaks they give back the text, a last
// break included: a file changes only where it was edited.
std::vector<std::string> lines_of(std::string_view text) {
  std::vector<std::string> lines;
  for (std::size_t at = 0;;) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    lines.emplace_back(text.substr(at, end - at));
    if (end == text.size())
      return lines;
    at = end + 1;
  }
}

std::string joined(const std::vector<std::string> &lines) {
  std::string text = lines.front();
  for (const std::string &line : std::span(lines).subspan(1))
    text.append(1, '\n').append(line);
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

// A key that changes the text: a break, an indent, or the selection or a character gone.
bool changed_by(Document &document, std::string_view key) {
  const auto [from, to] = selection(document);
  if (key == "enter")
    split(document);
  else if (key == "tab")
    insert(document, std::string(indent - document.caret.column % indent, ' '));
  else if ((key == "backspace" || key == "delete") && from != to)
    erase(document, from, to);
  else if (key == "backspace")
    erase(document, before(document, document.caret), document.caret);
  else if (key == "delete")
    erase(document, document.caret, after(document, document.caret));
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

void Text::bind(VP::Bind &node) {
  _open =
      node.command("open <file>", "opens a file in the editor, as a tab, or shows it");
  _write = node.command("write", "saves the file the editor shows");
  _close =
      node.command("close", "closes the file the editor shows, unless it has changes");
  _discard =
      node.command("discard", "closes the file the editor shows, dropping its changes");
  _find = node.command("find <value>...",
                       "selects the next place the file shown holds the words, joined by "
                       "single blanks, after the caret, and from its start past its end");
  _name = node.name();
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
  const Grid grid = this->grid();
  if (!focused())
    _shift = _control = false;
  for (const VP::Event &event : _typed->to(_name))
    if (!escape(frame, event, grid))
      edit(frame.input(), event, grid);
  point(frame, grid);
  lay_out(frame, grid);
  if (!_unsaved.empty())
    frame.hold(_unsaved);
}

Grid Text::grid() const {
  const glm::ivec2 cell(_font->cell);
  const glm::ivec2 room = glm::ivec2(_area->extent) - 2 * margin;
  if (cell.x <= 0 || cell.y <= 0 || room.x < cell.x || room.y < cell.y)
    return {};
  return {.origin = _area->offset + margin,
          .cell = cell,
          .rows = std::min(static_cast<std::size_t>(room.y / cell.y), row_room),
          .columns = static_cast<std::size_t>(room.x / cell.x)};
}

bool Text::focused() const {
  return _typed->focus == _name;
}

// In the order the events came, so a press is tested where the pointer was then. A drag
// moves the caret and leaves the selection's start where it was pressed.
void Text::point(VP::Cook &frame, const Grid &grid) {
  for (const VP::Event &event : frame.input().events()) {
    const bool inside = VP_VIEW::contains(*_area, _pointer);
    if (event.kind == VP::Event::Kind::pointer) {
      _pointer = event.at;
      if (_dragging && grid.rows > 0) {
        _documents[_shown].caret = spot_at(_documents[_shown], grid);
        _follow = true;
      }
    } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
      _dragging = event.down && inside && grid.rows > 0 && !_documents.empty();
      if (event.down && inside)
        press(frame, grid);
    } else if (event.kind == VP::Event::Kind::wheel && inside && !_documents.empty()) {
      scroll(event.turn.y);
    }
  }
}

// A press gives it the focus, and puts the caret where it was.
void Text::press(VP::Cook &frame, const Grid &grid) {
  if (!focused())
    frame.commands().send(std::format("focus {}", _name));
  if (!_dragging)
    return;
  Document &document = _documents[_shown];
  document.caret = document.anchor = spot_at(document, grid);
  _follow = true;
}

// The wheel turns up by a positive step, which shows the rows above.
void Text::scroll(float turn) {
  Document &document = _documents[_shown];
  const auto rows = static_cast<std::ptrdiff_t>(std::lround(-turn * wheel_rows));
  const auto last = static_cast<std::ptrdiff_t>(document.lines.size()) - 1;
  document.top = static_cast<std::size_t>(std::clamp(
      static_cast<std::ptrdiff_t>(document.top) + rows, std::ptrdiff_t{0}, last));
}

// The spot nearest the pointer: the row it is on, and the boundary between characters
// closest to it, so a press between two puts the caret between them.
Spot Text::spot_at(const Document &document, const Grid &grid) const {
  const glm::vec2 cells = (_pointer - glm::vec2(grid.origin)) / glm::vec2(grid.cell);
  const auto row = static_cast<std::size_t>(std::max(0.0f, std::floor(cells.y)));
  const auto column = static_cast<std::size_t>(std::max(0.0f, std::round(cells.x)));
  return at(document, document.top + row, document.left + column);
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
// held, changes the text, or with control copies, cuts or pastes; any other does
// nothing here. With no room it takes none.
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
  if (event.kind == VP::Event::Kind::text) {
    for (const char typed : event.name)
      if (typed >= first_printable && typed <= last_printable)
        insert(document, std::string_view(&typed, 1));
  } else if (const std::optional<Spot> to = moved(document, event.name, grid)) {
    document.caret = *to;
    if (!_shift)
      document.anchor = *to;
  } else if (!(_control && clip(input, document, event.name)) &&
             !changed_by(document, event.name)) {
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
// or end. Without shift, left and right leave a selection at its start or its end.
std::optional<Spot>
Text::moved(const Document &document, std::string_view key, const Grid &grid) const {
  const Spot caret = document.caret;
  const auto [from, to] = selection(document);
  const bool selected = from != to && !_shift;
  const std::size_t rows = key.starts_with("page") ? grid.rows : 1;
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
  follow(document, grid);
  if (std::exchange(document.restyle, false))
    find_comments(document);
  const std::size_t rows = std::min(grid.rows, document.lines.size() - document.top);
  const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
  rects[0] = {.offset = _area->offset,
              .extent = _area->extent,
              .role = VP_VIEW::role("background")};
  std::size_t count = 1 + select(document, grid, rows, rects.subspan(1));
  const Spot caret = document.caret;
  if (focused() && caret.row >= document.top && caret.row < document.top + rows &&
      caret.column >= document.left && caret.column < document.left + grid.columns)
    rects[count++] = {
        .offset = cell_at(grid, caret.row - document.top, caret.column - document.left),
        .extent = {caret_width, static_cast<std::uint32_t>(grid.cell.y)},
        .role = VP_VIEW::role("accent")};
  frame.write(_rects, count);
  write_lines(frame, document, grid, rows);
}

// The first row and column shown, scrolled so the caret shows once it moved; the wheel
// alone may leave it out of view.
void Text::follow(Document &document, const Grid &grid) {
  document.top = std::min(document.top, document.lines.size() - 1);
  if (!std::exchange(_follow, false))
    return;
  const Spot caret = document.caret;
  document.top = std::clamp(
      document.top, caret.row + 1 - std::min(caret.row + 1, grid.rows), caret.row);
  document.left = std::clamp(document.left,
                             caret.column + 1 - std::min(caret.column + 1, grid.columns),
                             caret.column);
}

// A Rect a row shown, behind the part of it selected, a row's break as one cell more.
std::size_t Text::select(const Document &document,
                         const Grid &grid,
                         std::size_t rows,
                         std::span<VP_VIEW::Rect> rects) const {
  const auto [from, to] = selection(document);
  std::size_t count = 0;
  for (std::size_t shown = 0; shown < rows && from != to; ++shown) {
    const std::size_t row = document.top + shown;
    if (row < from.row || row > to.row)
      continue;
    const std::size_t first = std::max(row == from.row ? from.column : 0, document.left);
    const std::size_t end =
        std::min(row == to.row ? to.column : document.lines[row].size() + 1,
                 document.left + grid.columns);
    if (first < end)
      rects[count++] = {.offset = cell_at(grid, shown, first - document.left),
                        .extent = extent_of(grid, end - first),
                        .role = VP_VIEW::role("border")};
  }
  return count;
}

// Whether each row starts inside a block comment, from the file's top, once an edit
// came, so a frame scans only the rows it shows.
void Text::find_comments(Document &document) {
  document.commented.assign(document.lines.size(), false);
  bool commented = false;
  for (std::size_t row = 0; row < document.lines.size(); ++row) {
    document.commented[row] = commented;
    commented = scan(document.lines[row],
                     document.syntax,
                     commented,
                     [](std::size_t, std::size_t, std::uint32_t) {});
  }
}

// A Label a run of one role in a row shown, holding the bytes of it that fit from the
// first column shown; what the room lacks is cut.
void Text::write_lines(VP::Cook &frame,
                       const Document &document,
                       const Grid &grid,
                       std::size_t rows) {
  const std::span<VP_VIEW::Label> labels = frame.write(_labels);
  const std::span<VP_VIEW::Character> characters = frame.write(_characters);
  std::uint32_t count = 0;
  std::uint32_t next = 0;
  for (std::size_t shown = 0; shown < rows; ++shown) {
    const std::size_t row = document.top + shown;
    const std::string_view line = document.lines[row];
    const std::size_t right = document.left + grid.columns;
    const std::uint32_t first = count; // the row's first Label
    bool cut = false; // a run the room lacked, so no later one joins a Label past it
    scan(line,
         document.syntax,
         document.commented[row],
         [&](std::size_t from, std::size_t to, std::uint32_t role) {
           from = std::max(from, document.left);
           to = std::min({to, right, from + characters.size() - next});
           if (cut || from >= to)
             return;
           // A run in the role of the one before it on the row joins its Label.
           if (count == first || labels[count - 1].role != role) {
             if (count == labels.size()) {
               cut = true;
               return;
             }
             labels[count++] = {.offset = cell_at(grid, shown, from - document.left),
                                .role = role,
                                .first = next};
           }
           VP_VIEW::Label &label = labels[count - 1];
           for (std::size_t at = from; at < to; ++at)
             characters[next++] = {.code = static_cast<unsigned char>(line[at]),
                                   .label = count - 1};
           label.count += static_cast<std::uint32_t>(to - from);
           label.extent = extent_of(grid, label.count);
         });
  }
  frame.write(_labels, count);
  frame.write(_characters, next); // the used length, which glyphs draws
}

void Text::command(VP::Call &call) {
  if (call.is(_open)) {
    open(call, call.arguments().front());
    return;
  }
  if (_documents.empty())
    throw std::runtime_error("no file is open");
  const Document &document = _documents[_shown];
  if (call.is(_write))
    write(call);
  else if (call.is(_close) && document.changed)
    throw std::runtime_error(std::format(
        "{} has changes: write saves them, and discard drops them", label_of(document)));
  else if (call.is(_close) || call.is(_discard))
    close();
  else if (call.is(_find))
    find(call);
}

// Read once, when it opens, so what was typed is never read over.
void Text::open(VP::Call &call, std::string_view path) {
  const auto found = std::ranges::find(_documents, path, &Document::path);
  const auto shown = static_cast<std::size_t>(found - _documents.begin());
  if (found == _documents.end())
    _documents.push_back({.path = std::string(path),
                          .lines = lines_of(call.files().read(path)),
                          .syntax = syntax_of(path)});
  _shown = shown;
  _dragging = false;
  _follow = true;
  name_tabs();
}

void Text::write(VP::Call &call) {
  Document &document = _documents[_shown];
  call.files().save(document.path, joined(document.lines));
  document.changed = false;
  name_tabs();
  call.reply(std::format("wrote {}", document.path));
}

// The file after it shows, or the one before at the end.
void Text::close() {
  _documents.erase(_documents.begin() + static_cast<std::ptrdiff_t>(_shown));
  _shown = std::min(_shown, _documents.empty() ? 0 : _documents.size() - 1);
  _dragging = false;
  _follow = true;
  name_tabs();
}

// From the selection's end to the file's end, then from its start, so a find repeated
// steps through every place in turn.
void Text::find(VP::Call &call) {
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
  std::string changed;
  std::size_t count = 0;
  for (std::size_t at = 0; at < _documents.size(); ++at) {
    _tabs->push_back({.label = label_of(_documents[at]),
                      .command = std::format("open {}", _documents[at].path),
                      .current = at == _shown});
    if (!_documents[at].changed)
      continue;
    changed.append(count++ == 0 ? "" : ", ").append(_tabs->back().label);
  }
  _unsaved = count == 0 ? std::string()
                        : std::format("{} {} changes: write saves the file shown, and "
                                      "discard drops its changes",
                                      changed,
                                      count == 1 ? "has" : "have");
}

} // namespace

VP_OPERATORS(registry) {
  registry.add<Text>("Text");
}
