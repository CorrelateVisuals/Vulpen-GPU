#pragma once

#include "runtime/Operator.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace VP_VIEW {

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
  // Shows the file, opening it first when it holds it not.
  virtual void show(VP::FilePort &files, std::string_view path) = 0;
  virtual void write(VP::Call &call) = 0;
  // The file named, which it holds, or the one it shows; with changes only by force.
  virtual void close(VP::Call &call, std::string_view path, bool force) = 0;
  virtual void find(VP::Call &call) = 0;

protected:
  ~Editor() = default;
};

// The editors of a view, as a chain of text nodes hands them on, each adding itself to
// those it was handed.
using Editors = std::vector<Editor *>;

} // namespace VP_VIEW
