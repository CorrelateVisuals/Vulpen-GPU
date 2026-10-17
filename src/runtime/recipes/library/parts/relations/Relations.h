#pragma once

#include "contracts/Relation.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Only Relations.cpp includes this header, so the class stays in the unnamed namespace,
// and two copies of the part in one binary never clash.
namespace {

// A name a node's C++ binds to its shaders' pass block, as upload<glm::vec2>("corners"),
// by the file and the line that bind it, which the shader declaring it links to.
struct Bound {
  std::filesystem::path file;
  std::string name;
  std::uint32_t line = 0;
};

// Finds what links the files of the view the graph shows, the current one, or of the
// folder its folder param names, whose inside the graph shows, which open and the graph
// set while the ide runs: a file including or importing another, a CMake file naming
// one, a document linking one or citing an id a document of its docs param's folder
// defines, a manifest naming a recipe it uses, a node's C++ binding a name its shader's
// pass block declares; and hands them to the graph as Relations,
// with the folder a level deep as a view's nodes, hidden ones too, and the top of its
// repository. No compiler is asked: it reads the lines that say so, as they are on disk,
// through the file port, again as what is shown changes, and every two seconds while
// what it reads is small, a little a frame.
class Relations final : public VP::Operator {
public:
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;

private:
  void start(VP::FilePort &files, const VP::View *view);
  bool step(VP::FilePort &files, std::size_t bytes);
  void publish_tree();
  void publish(VP::Cook &frame);

  VP_VIEW::Relations *_relations = nullptr;
  std::string _folder;
  std::string _docs; // the folder whose documents define the ids a file cites
  std::string _shown; // the view read last
  const void *_read_view = nullptr;
  bool _stale = true;
  std::uint64_t _frames = 0; // since the last reading, or the reading under way took
  std::size_t _bytes = 0;    // the last reading read
  // A reading under way, which may take frames: the folder's tree and its top, the folders
  // below it to walk and the next of them, the files to read and the next of them, the
  // folders listed, the ids the documents define, the relations found so far, and what
  // was read, a folder listed counting as a page.
  bool _reading = false;
  VP::View _tree;
  std::string _top;
  std::vector<std::filesystem::path> _walk;
  std::size_t _walked = 0;
  std::vector<std::filesystem::path> _sources;
  std::size_t _next = 0;
  std::map<std::string, std::vector<std::string>> _listed;
  std::map<std::string, std::string, std::less<>> _ids;
  std::vector<VP_VIEW::Relation> _found;
  std::size_t _read = 0;
  // The seam between a node's C++ and its shaders, which only both files say together:
  // what C++ binds, and the names each shader's pass block declares, by the shader.
  std::vector<Bound> _bound;
  std::map<std::filesystem::path, std::vector<std::string>> _declared;
};

} // namespace
