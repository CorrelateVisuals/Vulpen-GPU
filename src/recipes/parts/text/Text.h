#pragma once

#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "shared/Scroll.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Only Text.cpp includes this header, so the class stays in the unnamed namespace, and
// two copies of the part in one binary never clash.
namespace {

// How a file's text is told apart: code, as C++ and GLSL, by its kinds of words; a
// manifest or an .ini by its keys; any other not at all.
enum class Syntax : std::uint8_t { plain, code, keyed };

// A place in a file: a row, and the byte of it where a character starts.
struct Spot {
  std::size_t row = 0;
  std::size_t column = 0;

  auto operator<=>(const Spot &) const = default;
};

// A file open in the editor, as typed so far. It keeps its caret, the selection and the
// part of it shown while another file shows.
struct Document {
  std::string path; // absolute, as open was given it
  std::vector<std::string> lines;
  Syntax syntax = Syntax::plain;
  std::vector<bool> commented; // whether each row starts inside a block comment
  bool restyle = true;         // an edit came since commented was found
  Spot caret;
  Spot anchor;          // where the selection starts; the caret itself for none
  Scroll scroll;        // the rows shown
  std::size_t left = 0; // the first column shown
  bool changed = false; // since it was opened or last written
};

// Where the text goes in the area, in pixels, and how many rows and columns of it fit;
// no rows when it has no room, or no font.
struct Grid {
  glm::ivec2 origin{};
  glm::ivec2 cell{};
  std::size_t rows = 0;
  std::size_t columns = 0;
};

// The text a person reads and edits, loaded and saved through the file port. It writes
// Labels and Rects and draws nothing, so every text area is this part plus the drawing
// parts and a fix to text reaches them all. Find is one of its commands, so a find bar
// is only a front end on it.
//
// It holds the files open, shows one, and hands a panel a tab for each, whose press
// shows it again; while one has changes, quit refuses, naming it. The wheel scrolls it,
// as does a drag on the bar at its right edge. It takes keys from the keys part while it
// has the focus, which a press in its area gives it, and none while it has no room. The
// font draws ASCII, so a character past it shows as a blank cell a byte, but the caret
// steps over it whole and no edit ever splits it.
class Text final : public VP::Operator {
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;
  void command(VP::Call &call) override;

  Grid grid() const;
  bool focused() const;
  void point(VP::Cook &frame, const Grid &grid);
  void press(VP::Cook &frame, const Grid &grid);
  Spot spot_at(const Document &document, const Grid &grid) const;
  bool escape(VP::Cook &frame, const VP::Event &event, const Grid &grid);
  void edit(VP::InputPort &input, const VP::Event &event, const Grid &grid);
  bool clip(VP::InputPort &input, Document &document, std::string_view key) const;
  std::optional<Spot> moved(const Document &document,
                            std::string_view key,
                            const Grid &grid) const;
  void lay_out(VP::Cook &frame, const Grid &grid);
  void follow(Document &document, const Grid &grid);
  static void find_comments(Document &document);
  std::size_t select(const Document &document,
                     const Grid &grid,
                     std::size_t rows,
                     std::span<VP_VIEW::Rect> rects) const;
  void write_lines(VP::Cook &frame,
                   const Document &document,
                   const Grid &grid,
                   std::size_t rows);
  void open(VP::Call &call, std::string_view path);
  void write(VP::Call &call);
  void close();
  void find(VP::Call &call);
  void name_tabs();

  VP::Command _open;
  VP::Command _write;
  VP::Command _close;
  VP::Command _discard;
  VP::Command _find;
  std::string _name; // as the focus names it
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  VP_VIEW::Items *_tabs = nullptr; // a tab a file open, for a panel
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels;
  VP::Upload<VP_VIEW::Character> _characters;
  std::vector<Document> _documents;
  // What the editor holds while files have changes, so quit keeps the run; named with the
  // tabs, from the same labels, so a frame allocates nothing for it (CPP10).
  std::string _unsaved;
  std::size_t _shown = 0; // which of them shows
  glm::vec2 _pointer{};   // as the events so far left it
  bool _dragging = false; // a selection, from a press in the area until it is let go
  bool _shift = false;    // held, so a key that moves the caret selects
  bool _control = false;  // held, so c, x and v copy, cut and paste
  bool _follow = false;   // the caret moved, so the next layout scrolls it into view
};

} // namespace
