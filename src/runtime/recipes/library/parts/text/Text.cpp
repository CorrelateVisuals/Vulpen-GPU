#include "Text.h"

#include "contracts/Palette.h"
#include "shared/Words.h"

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
// The ground, a selection Rect a row shown, the caret and the scroll bars' thumbs.
constexpr std::uint32_t rect_room = row_room + 4;
constexpr std::size_t indent = 2; // Tab types spaces to a multiple of this column
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

// A key that changes the text: a break, an indent, or the selection or a character gone;
// with control, backspace takes the word before the caret, or at a row's start its break.
bool changed_by(Document &document, std::string_view key, bool control) {
  const auto [from, to] = selection(document);
  const Spot caret = document.caret;
  if (key == "enter")
    split(document);
  else if (key == "tab")
    insert(document, std::string(indent - caret.column % indent, ' '));
  else if ((key == "backspace" || key == "delete") && from != to)
    erase(document, from, to);
  else if (key == "backspace" && control && caret.column > 0)
    erase(document,
          {caret.row, word_before(document.lines[caret.row], caret.column)},
          caret);
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
  _write = node.command("write", "saves the file the editor shows");
  _close = node.command("close [<file>]",
                        "closes a file the editor holds, or with none the one it shows, "
                        "unless it has changes");
  _close_force = node.command("close force [<file>]",
                              "closes a file the editor holds, or with none the one it "
                              "shows, dropping its changes");
  _find = node.command("find <value>...",
                       "selects the next place the file shown holds the words, joined by "
                       "single blanks, after the caret, and from its start past its end");
  _name = node.name();
  _view = node.view_name();
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
      open(frame.files(), _file);
    } catch (const std::runtime_error &failure) {
      frame.log(VP::Level::error, failure.what());
    }
  }
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
  lay_out(frame, grid);
  if (!_unsaved.empty())
    frame.hold(_unsaved);
}

// The text keeps clear of the scroll bars.
Grid Text::grid() const {
  const glm::ivec2 cell(_font->cell);
  const glm::ivec2 room = glm::ivec2(_area->extent) - 2 * margin - Scroll::width;
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

// The spot nearest the pointer: the row it is on, and the boundary between characters
// closest to it, so a press between two puts the caret between them; or with character,
// the start of the character it is on, as a double click's word is found from.
Spot Text::spot_at(const Document &document, const Grid &grid, bool character) const {
  const glm::vec2 cells = (_pointer - glm::vec2(grid.origin)) / glm::vec2(grid.cell);
  const auto row = static_cast<std::size_t>(std::max(0.0f, std::floor(cells.y)));
  const auto column = static_cast<std::size_t>(
      std::max(0.0f, character ? std::floor(cells.x) : std::round(cells.x)));
  return at(document, document.scroll.first() + row, document.columns.first() + column);
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
      if (typed >= VP_VIEW::first_printable && typed <= VP_VIEW::last_printable)
        insert(document, std::string_view(&typed, 1));
  } else if (const std::optional<Spot> to = moved(document, event.name, grid)) {
    document.caret = *to;
    if (!_shift)
      document.anchor = *to;
  } else if (!(_control && clip(input, document, event.name)) &&
             !changed_by(document, event.name, _control)) {
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
      caret.column < left + grid.columns)
    rects[count++] = {
        .offset = cell_at(grid, caret.row - top, caret.column - left),
        .extent = {caret_width, static_cast<std::uint32_t>(grid.cell.y)},
        .role = VP_VIEW::role("accent")};
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

void Text::command(VP::Call &call) {
  if (std::exchange(_opening, false))
    open(call.files(), _file);
  if (_documents.empty())
    throw std::runtime_error("no file is open");
  if (call.is(_write))
    write(call);
  else if (call.is(_close) || call.is(_close_force))
    close(call, call.is(_close_force));
  else if (call.is(_find))
    find(call);
}

// Read once, when it opens, so what was typed is never read over.
void Text::open(VP::FilePort &files, std::string_view path) {
  const auto found = std::ranges::find(_documents, path, &Document::path);
  const auto shown = static_cast<std::size_t>(found - _documents.begin());
  if (found == _documents.end())
    _documents.push_back({.path = std::string(path),
                          .lines = lines_of(files.read(path)),
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
// The file named, as a tab's close mark names it, or the one shown; one with changes only
// by force, so nothing typed goes unasked. The file shown keeps showing, and when it is
// the one closed, the one after it, or else the one before.
void Text::close(VP::Call &call, bool force) {
  std::size_t at = _shown;
  if (!call.arguments().empty()) {
    const auto found =
        std::ranges::find(_documents, call.arguments().front(), &Document::path);
    if (found == _documents.end())
      throw std::runtime_error(
          std::format("{} is not open in the editor", call.arguments().front()));
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
                      .current = at == _shown,
                      .close = std::format("close {}", _documents[at].path)});
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
