#pragma once

#include "runtime/Operator.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VP_VIEW {

// Which files beside one a close takes: those left of it, those right of it, or all of
// them with it.
enum class Beside : std::uint8_t { left, right, all };

// An editor a text part is, as the text node that answers the editor's commands reaches
// it: a command has one owner, so one node registers them and acts on the editor in
// focus, or the one in focus last, however many editors a view seats. A file is open in
// one editor at a time, so a file a command names finds its editor.
class Editor {
public:
  // The text node, as the focus names it.
  virtual const std::string &name() const = 0;
  // Whether it holds the file, by absolute path; how many it holds, and the one it shows,
  // empty for none, and whether that one has changes.
  virtual bool holds(std::string_view path) const = 0;
  virtual std::size_t count() const = 0;
  virtual std::string_view shown() const = 0;
  virtual bool changed() const = 0;
  // Shows the file, opening it first when it holds it not, with the file port's watch of
  // it, which the node whose param named it took as it bound.
  virtual void show(VP::FilePort &files, std::string_view path, VP::File watched) = 0;
  // The file it shows; one moved, deleted or changed on disk since it was read only by
  // force.
  virtual void write(VP::Call &call, bool force) = 0;
  // The file named, which it holds, or the one it shows; with changes only by force.
  virtual void close(VP::Call &call, std::string_view path, bool force) = 0;
  // The files left of the one named, which it holds, or of the one it shows, or right of
  // it, or all of them, but those with changes, which it names.
  virtual void close_beside(VP::Call &call, std::string_view path, Beside beside) = 0;
  virtual void find(VP::Call &call) = 0;
  // Puts the first file it holds where the second is, those between moving over a place,
  // as a tab dragged onto another moves among its strip's.
  virtual void move(std::string_view path, std::string_view to) = 0;
  // The rows of the file it shows, as typed so far, and the caret's row and the byte of
  // it where it stands, as jump reads the name there; none, and the first, while it
  // shows no file.
  virtual std::span<const std::string> rows() const = 0;
  virtual std::pair<std::size_t, std::size_t> caret() const = 0;
  // Shows the file, opening it first when it holds it not, a run of a row selected and
  // scrolled into view, as jump shows where a name comes from.
  virtual void reveal(VP::FilePort &files,
                      std::string_view path,
                      std::size_t row,
                      std::size_t column,
                      std::size_t length) = 0;
  // The file it holds at a place among its tabs, counted from the left, of the count it
  // holds, so the docking part keeps each editor's files between runs. Last, so a part
  // built before it reaches the others where it found them.
  virtual std::string_view file(std::size_t index) const = 0;

protected:
  ~Editor() = default;
};

// The editors of a view, as a chain of text nodes hands them on, each adding itself to
// those it was handed.
using Editors = std::vector<Editor *>;

} // namespace VP_VIEW
