#pragma once

#include "contracts/Relation.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Only Relations.cpp includes this header, so the class stays in the unnamed namespace,
// and two copies of the part in one binary never clash.
namespace {

// A name a node's C++ binds, as upload<glm::vec2>("corners"), by the file and the line
// that bind it: in its shaders' pass block, which the shader declaring it takes, or a
// port of a C++ connection, input or output; and whether the data leaves the C++ there,
// as an upload's, or reaches it, as a readback's.
struct Bound {
  std::filesystem::path file;
  std::string name;
  std::uint32_t line = 0;
  bool sends = false;
  bool pass = false;
};

// A member a shader's pass block declares, by its name and its line.
struct Declared {
  std::string name;
  std::uint32_t line = 0;
};

// A connection a manifest makes, by its name: its writer's port and its readers', as the
// manifest writes them, and the line naming the writer.
struct Wired {
  std::filesystem::path manifest;
  std::string name;
  std::string from;
  std::vector<std::string> to;
  std::uint32_t line = 0;
};

// What a stage of a draw writes out to the next stage, or reads in from the one before,
// as out vec3 color, by its location, or its name where it gives none, and its line.
struct Varying {
  std::string name;
  std::optional<std::uint32_t> location;
  std::uint32_t line = 0;
  bool out = false;
};

// The names each folder looked in during a reading holds, which Relations.cpp defines.
class Listed;

// What the files of a folder bind, declare in their pass blocks and pass from stage to
// stage, as only the files of one folder relate through them, a node's folder holding
// its C++ and its shaders (RV08).
struct InFolder {
  std::vector<const Bound *> bound;
  std::vector<std::pair<const std::filesystem::path *, const std::vector<Declared> *>>
      declared;
  std::vector<std::pair<const std::filesystem::path *, const std::vector<Varying> *>>
      varyings;
};

// The relations a reading of a folder found, kept to show at once when it shows again.
struct Known {
  std::string folder;
  std::vector<VP_VIEW::Relation> relations;
};

// Finds what links the files of the view the graph shows, the current one, or of the
// folder its folder param names, whose inside the graph shows, which open and the graph
// set while the ide runs: a file including or importing another, a CMake file naming
// one, a document linking one or citing an id a document of its docs param's folder
// defines, a manifest naming a recipe it uses; and what the engine carries between files:
// a node's C++ binding a name its shader's pass block declares, a stage of a draw reading
// what the stage before it writes, C++ loading a shader by its SPIR-V's name, and in a
// folder shown, a manifest's connection between the files of its nodes; and hands them
// to the graph as Relations,
// with the folder a level deep as a view's nodes, hidden ones too, and the top of its
// repository. No compiler is asked: it reads the lines that say so, as they are on disk,
// through the file port, again as what is shown changes, and every two seconds while
// what it reads is small, a little a frame; a folder read lately shows what was found
// then at once, while it is read again.
class Relations final : public VP::Operator {
public:
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;

private:
  void start(VP::FilePort &files, const VP::View *view);
  bool step(VP::FilePort &files, std::chrono::steady_clock::time_point until);
  bool read_slice(VP::FilePort &files, Listed &listed);
  bool recall();
  void index_folders();
  void publish_tree();
  void publish(VP::Cook &frame);
  void relate_seams();
  void relate_stages();
  void relate_wires();
  std::optional<std::pair<std::filesystem::path, std::uint32_t>>
  port_file(const std::filesystem::path &folder,
            std::string_view port,
            bool writes) const;

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
  bool _holding = false; // a folder shown anew, which waits for its relations
  VP::View _tree;
  std::string _top;
  std::vector<std::filesystem::path> _walk;
  std::size_t _walked = 0;
  std::vector<std::filesystem::path> _sources;
  std::size_t _next = 0;
  // The file being read, how much of it is, and how many of its lines.
  std::string _text;
  std::size_t _sliced = 0;
  std::uint32_t _lines = 0;
  std::map<std::string, std::vector<std::string>> _listed;
  std::map<std::string, std::string, std::less<>> _ids;
  std::vector<VP_VIEW::Relation> _found;
  std::size_t _read = 0;
  // The seam between a node's C++ and its shaders, which only both files say together:
  // what C++ binds, and the names each shader's pass block declares, by the shader.
  std::vector<Bound> _bound;
  std::map<std::filesystem::path, std::vector<Declared>> _declared;
  // And between a draw's stages, which only both shaders say together: what each stage
  // writes out and reads in, by the shader.
  std::map<std::filesystem::path, std::vector<Varying>> _varyings;
  // And between the nodes of a folder shown, which its manifests join.
  std::vector<Wired> _wired;
  // Those of each folder, while a reading is handed on.
  std::map<std::filesystem::path, InFolder> _in_folder;
  // The folders read last, the newest last.
  std::vector<Known> _known;
};

} // namespace
