#pragma once

#include "contracts/Editors.h"
#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "shared/Caret.h"
#include "shared/Labels.h"
#include "shared/Paths.h"
#include "shared/Scroll.h"
#include "Sources.h"
#include "Syntax.h"

#include <compare>
#include <filesystem>
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

// A place in a file: a row, and the byte of it where a character starts.
struct Spot {
  std::size_t row = 0;
  std::size_t column = 0;

  auto operator<=>(const Spot &) const = default;
};

// An edit, as control with z takes it back and control with shift and z makes it again:
// from the row first on, the rows it changed as they were and as it left them, and the
// caret and the selection's start before it and after it; whether it is characters typed,
// which the next typed at its end joins.
struct Change {
  std::size_t first = 0;
  std::vector<std::string> before;
  std::vector<std::string> after;
  Spot caret_before;
  Spot anchor_before;
  Spot caret_after;
  Spot anchor_after;
  bool typed = false;
};

// A file open in the editor, as typed so far. It keeps its caret, the selection and the
// part of it shown while another file shows, and its edits, to take back.
struct Document {
  std::string path; // absolute, as open was given it
  std::vector<std::string> lines;
  bool crlf = false; // its lines end in \r\n, as Windows writes them, which write keeps
  Syntax syntax = Syntax::plain;
  std::vector<Open> inside; // what each row starts in: a block comment, or a string
  std::size_t widest = 0;   // the widest row's bytes
  bool restyle = true;      // an edit came since inside and widest were found
  Spot caret;
  Spot anchor;   // where the selection starts; the caret itself for none
  Scroll scroll; // the rows shown
  Scroll columns{Scroll::Axis::across};
  bool changed = false; // since it was opened or last written
  std::vector<Change> undone; // the edits so far, the newest last
  std::vector<Change> redone; // those taken back since, the newest last
  // How many edits undone held as the file was read or last written, so changed says
  // whether the text differs from the file; none once that text can come back no more.
  std::optional<std::size_t> saved = 0;
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
// It holds the files open, shows one, and hands a panel a tab for each, whose press shows
// it again; while one has changes, quit refuses, naming it. Several text nodes are
// several editors: each hands on, as editors, those a connection to its before brings and
// itself, and the one whose editors reach no text node's before registers write, close
// and find, acting on the editor in focus, or the one in focus last, and takes open's
// file param for them all, showing a file in the editor holding it, else in that one, and
// jump, which F12 sends, showing where the name at the caret comes from. The wheel
// scrolls it, as does a drag on the bar at its right edge, and across, turned sideways or
// with shift, as does a drag on the bar at its bottom edge; a double click selects a
// word, and one more press the row. It takes keys from the keys part while it has the
// focus, which a press in its area gives it, and none while it has no room. The font
// draws ASCII, so a character past it shows as a blank cell a byte, but the caret steps
// over it whole and no edit ever splits it.
class Text final : public VP::Operator, public VP_VIEW::Editor {
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;
  void command(VP::Call &call) override;

  const std::string &name() const override;
  bool holds(std::string_view path) const override;
  std::size_t count() const override;
  std::string_view shown() const override;
  bool changed() const override;
  void show(VP::FilePort &files, std::string_view path) override;
  void write(VP::Call &call) override;
  void close(VP::Call &call, std::string_view path, bool force) override;
  void close_beside(VP::Call &call, std::string_view path, VP_VIEW::Beside beside) override;
  void find(VP::Call &call) override;
  void move(std::string_view path, std::string_view to) override;
  std::span<const std::string> rows() const override;
  std::pair<std::size_t, std::size_t> caret() const override;
  void reveal(VP::FilePort &files,
              std::string_view path,
              std::size_t row,
              std::size_t column,
              std::size_t length) override;
  void jump(VP::Call &call);
  std::vector<Source> sources(VP::FilePort &files,
                              Listed &listed,
                              const std::string &file,
                              std::span<const std::string> rows,
                              std::string_view name) const;
  void show_source(VP::Call &call, VP_VIEW::Editor &editor, const Source &source);

  VP_VIEW::Editor &target();
  VP_VIEW::Editor *holder(std::string_view path) const;
  VP_VIEW::Editor *holder_named(std::string_view name) const;
  void route(VP::FilePort &files);

  Grid grid() const;
  bool focused() const;
  void point(VP::Cook &frame, const Grid &grid);
  void press(VP::Cook &frame, const Grid &grid, std::uint32_t presses);
  Spot spot_at(const Document &document, const Grid &grid, bool character = false) const;
  void drift(Document &document, const Grid &grid);
  bool escape(VP::Cook &frame, const VP::Event &event, const Grid &grid);
  void edit(VP::InputPort &input, const VP::Event &event, const Grid &grid);
  bool clip(VP::InputPort &input, Document &document, std::string_view key) const;
  std::optional<Spot> moved(const Document &document,
                            std::string_view key,
                            const Grid &grid) const;
  void lay_out(VP::Cook &frame, const Grid &grid);
  void follow(Document &document);
  static void measure(Document &document);
  std::size_t select(const Document &document,
                     const Grid &grid,
                     std::size_t rows,
                     std::span<VP_VIEW::Rect> rects) const;
  void write_lines(VP::Cook &frame,
                   const Document &document,
                   const Grid &grid,
                   std::size_t rows);
  void open(VP::FilePort &files, std::string_view path);
  void name_tabs();

  VP::Command _write;
  VP::Command _close;
  VP::Command _close_force;
  VP::Command _close_left;
  VP::Command _close_right;
  VP::Command _close_all;
  VP::Command _find;
  VP::Command _move;
  VP::Command _jump;
  std::string _name; // as the focus names it
  std::string _view; // the one it runs in, where its file param is unset
  // Its own folder, in the library, from which jump reaches the engine's headers, as a
  // node's include path does, where a project's file includes one.
  std::filesystem::path _library;
  const VP_VIEW::Editors *_before = nullptr; // a connection's; none for none
  VP_VIEW::Editors *_editors = nullptr;      // those, then itself
  bool _head = false;                        // it answers the commands for all of them
  std::string _last; // the editor in focus last, by name, which the commands act on
  // What the file param names, from the node's folder; empty while it is not set.
  std::string _file;
  bool _opening = false; // the param named another file, which opens where the port is
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
  bool _sideways = false; // shift held, as the window says, so the wheel scrolls across
  bool _control = false;  // held, so c, x and v copy, cut and paste
  bool _follow = false;   // the caret moved, so the next layout scrolls it into view
  Caret _blink;           // the caret's mark, as it blinks
  // The name jump looked for last, and where it found it, which a jump from one of those
  // goes on among.
  std::string _jumped;
  std::vector<Source> _found_last;
};

} // namespace
