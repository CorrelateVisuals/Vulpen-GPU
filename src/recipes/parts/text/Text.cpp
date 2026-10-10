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

bool continues(char byte) {
  return (static_cast<unsigned char>(byte) & continuation_mask) == continuation;
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
  document.changed = true;
}

// Text without a break, typed at the caret in place of the selection.
void insert(Document &document, std::string_view text) {
  const auto [from, to] = selection(document);
  erase(document, from, to);
  document.lines[document.caret.row].insert(document.caret.column, text);
  document.caret.column += text.size();
  document.anchor = document.caret;
  document.changed = true;
}

// Enter: what follows the caret goes down a row, behind the spaces the row starts with,
// so a block of code keeps its indent.
void split(Document &document) {
  const auto [from, to] = selection(document);
  erase(document, from, to);
  const Spot caret = document.caret;
  std::string &line = document.lines[caret.row];
  const std::size_t spaces = std::min(line.find_first_not_of(' '), caret.column);
  std::string below = line.substr(0, spaces) + line.substr(caret.column);
  line.erase(caret.column);
  document.lines.insert(
      document.lines.begin() + static_cast<std::ptrdiff_t>(caret.row) + 1,
      std::move(below));
  document.caret = document.anchor = {caret.row + 1, spaces};
  document.changed = true;
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
  _labels = node.upload<VP_VIEW::Label>("labels", row_room);
  _characters = node.upload<VP_VIEW::Character>("characters", character_room);
  name_tabs();
}

// The keys first: the keys part hands on only those that came before this frame's first
// press, so a press moves the caret after them, as they came. A key let go while another
// part had the focus never came, so out of focus shift counts as up.
void Text::cook(VP::Cook &frame) {
  const Grid grid = this->grid();
  if (!focused())
    _shift = false;
  for (const VP::Event &event : _typed->to(_name))
    edit(event, grid);
  point(frame, grid);
  lay_out(frame, grid);
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

// Typed text goes in at the caret, and a key moves the caret, selecting while shift is
// held, or changes the text; any other does nothing here. With no room it takes none.
void Text::edit(const VP::Event &event, const Grid &grid) {
  if (event.kind == VP::Event::Kind::key && event.name == "shift")
    _shift = event.down;
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
  } else if (!changed_by(document, event.name)) {
    return;
  }
  _follow = true;
  if (document.changed != changed)
    name_tabs();
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

// A Label a row shown, holding the bytes of it that fit from the first column shown;
// what the room lacks is cut.
void Text::write_lines(VP::Cook &frame,
                       const Document &document,
                       const Grid &grid,
                       std::size_t rows) {
  const std::span<VP_VIEW::Label> labels = frame.write(_labels, rows);
  const std::span<VP_VIEW::Character> characters = frame.write(_characters);
  std::size_t next = 0;
  for (std::size_t shown = 0; shown < rows; ++shown) {
    const std::string_view line = document.lines[document.top + shown];
    const std::string_view seen =
        line.substr(std::min(document.left, line.size()), grid.columns);
    const std::size_t count = std::min(seen.size(), characters.size() - next);
    for (std::size_t at = 0; at < count; ++at)
      characters[next + at] = {.code = static_cast<unsigned char>(seen[at]),
                               .label = static_cast<std::uint32_t>(shown)};
    labels[shown] = {.offset = cell_at(grid, shown, 0),
                     .extent = extent_of(grid, grid.columns),
                     .role = VP_VIEW::role("text"),
                     .first = static_cast<std::uint32_t>(next),
                     .count = static_cast<std::uint32_t>(count)};
    next += count;
  }
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
    _documents.push_back(
        {.path = std::string(path), .lines = lines_of(call.files().read(path))});
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
  for (std::size_t at = 0; at < _documents.size(); ++at)
    _tabs->push_back({.label = label_of(_documents[at]),
                      .command = std::format("open {}", _documents[at].path),
                      .current = at == _shown});
}

} // namespace

VP_OPERATORS(registry) {
  registry.add<Text>("Text");
}
