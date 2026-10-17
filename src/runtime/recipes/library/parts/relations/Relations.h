#pragma once

#include "contracts/Relation.h"
#include "runtime/Operator.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Only Relations.cpp includes this header, so the class stays in the unnamed namespace,
// and two copies of the part in one binary never clash.
namespace {

// Finds what links the files of the view the graph shows, the current one, or of the
// folder its folder param names, which open sets while the ide runs: a file including or
// importing another, a CMake file naming one, a document linking one or citing an id a
// document of its docs param's folder defines, a manifest naming a recipe it uses; and
// hands them to the graph as Relations, with the folder as a view's nodes. No compiler
// is asked: it reads the lines that say so, as they are on disk, through the file port,
// again as what is shown changes, and every two seconds while what it reads is small.
class Relations final : public VP::Operator {
public:
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;

private:
  VP_VIEW::Relations *_relations = nullptr;
  std::string _folder;
  std::string _docs; // the folder whose documents define the ids a file cites
  std::string _shown; // the view read last
  const void *_read_view = nullptr;
  bool _stale = true;
  std::uint64_t _frames = 0; // since the last reading
  std::size_t _bytes = 0;    // the last reading read
};

} // namespace
