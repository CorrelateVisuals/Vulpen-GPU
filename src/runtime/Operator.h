#pragma once

#include "baseclasses/Log.h"

#include <glm/ext/vector_uint2_sized.hpp>
#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

// The runtime header every node's C++ includes: a node's behaviour and the general
// ports (commands, input, files, the terminal). Nothing here reaches Vulkan or the OS.
//
// A node's C++ gets names in and handles out, never an object that owns the GPU, so a
// swap never leaves it holding one that is gone. Between C++ nodes, objects pass by
// reference (docs/plans/native-cpp.md).

namespace VP {

// The engine, which a node may include by hand (docs/plans/native-cpp.md); declared
// here only, so a node that does not use it compiles as fast as one that never could.
class Engine;

// The graph a node reads. Only C++ that reads it includes runtime/View.h, so the rest
// compiles without its headers.
struct View;

// The GLSL name of each type C++ may put in a pass block; the loader compares it with
// the type the shader declares. glm's vectors are GLSL's, name for name.
template <class T> inline constexpr std::string_view glsl_type{};
template <> inline constexpr std::string_view glsl_type<float> = "float";
template <> inline constexpr std::string_view glsl_type<std::int32_t> = "int";
template <> inline constexpr std::string_view glsl_type<std::uint32_t> = "uint";
template <> inline constexpr std::string_view glsl_type<glm::vec2> = "vec2";
template <> inline constexpr std::string_view glsl_type<glm::vec3> = "vec3";
template <> inline constexpr std::string_view glsl_type<glm::vec4> = "vec4";
template <> inline constexpr std::string_view glsl_type<glm::ivec2> = "ivec2";
template <> inline constexpr std::string_view glsl_type<glm::ivec3> = "ivec3";
template <> inline constexpr std::string_view glsl_type<glm::ivec4> = "ivec4";
template <> inline constexpr std::string_view glsl_type<glm::uvec2> = "uvec2";
template <> inline constexpr std::string_view glsl_type<glm::uvec3> = "uvec3";
template <> inline constexpr std::string_view glsl_type<glm::uvec4> = "uvec4";

// The name of each pixel C++ may fill an image with; the loader compares it with the
// pixel the image's format takes, byte for byte. A 16-bit float is its bits, as
// glm::packHalf gives them.
template <class T> inline constexpr std::string_view pixel_type{};
template <> inline constexpr std::string_view pixel_type<std::uint8_t> = "uint8";
template <> inline constexpr std::string_view pixel_type<glm::u8vec2> = "u8vec2";
template <> inline constexpr std::string_view pixel_type<glm::u8vec4> = "u8vec4";
template <> inline constexpr std::string_view pixel_type<std::uint16_t> = "uint16";
template <> inline constexpr std::string_view pixel_type<glm::u16vec2> = "u16vec2";
template <> inline constexpr std::string_view pixel_type<glm::u16vec4> = "u16vec4";
template <> inline constexpr std::string_view pixel_type<float> = "float";
template <> inline constexpr std::string_view pixel_type<glm::vec2> = "vec2";
template <> inline constexpr std::string_view pixel_type<glm::vec4> = "vec4";

// A value in the node's pass block.
template <class T> struct Value {
  std::uint32_t offset = 0;
};

// One of the node's buffers, as the CPU sees it a frame after the GPU wrote it.
template <class T> struct Readback {
  std::uint32_t index = 0;
};

// One of the node's buffers, as the CPU writes it for the GPU to read that frame.
template <class T> struct Upload {
  std::uint32_t index = 0;
};

// An image the node's C++ fills once, a T a pixel, and shaders then sample.
template <class T> struct Texture {
  std::uint32_t index = 0;
};

// A command the node registered; its runs reach the node's command hook.
struct Command {
  std::uint32_t index = 0;
};

// A file the node opened while it bound, which the file port watches.
struct File {
  std::uint32_t index = 0;
};

// One member of a struct a buffer holds, by its name, GLSL type and offset. A C++ struct
// names each member once, in a static members() that lists them with VP_MEMBER, and the
// loader checks each against the shader's struct, so it cannot drift from it (RA03).
struct Member {
  std::string_view name;
  std::string_view type;
  std::size_t offset = 0;
};

template <class T> constexpr Member member(std::string_view name, std::size_t offset) {
  static_assert(!glsl_type<T>.empty(),
                "a struct's member is a float, int, uint or a glm vector of them");
  return {name, glsl_type<T>, offset};
}

// What a buffer's element is in C++: its size, and its GLSL type, or a struct's members.
struct Element {
  std::size_t size = 0;
  std::string_view type;
  std::span<const Member> members;
};

template <class T> Element element_of() {
  static_assert(std::is_trivially_copyable_v<T>);
  if constexpr (requires { T::members(); }) {
    static constexpr auto members = T::members();
    return {sizeof(T), {}, members};
  } else {
    static_assert(!glsl_type<T>.empty(),
                  "a buffer holds floats, ints, uints, glm vectors of them, or structs "
                  "whose members() names their members with VP_MEMBER");
    return {sizeof(T), glsl_type<T>, {}};
  }
}

// A C++ object of a type the engine never names, destroyed by the code that made it.
using Object = std::unique_ptr<void, void (*)(void *)>;

// What a connection between C++ nodes carries: the type, which both ends must name
// alike, and how to make one in the code of the module that asks, so the object's
// destructor goes with that module (docs/plans/native-cpp.md, rule 2).
struct Kind {
  const char *type = nullptr; // as typeid names it
  std::size_t size = 0;
  std::size_t align = 0;
  Object (*make)() = nullptr;
};

template <class T> Kind kind_of() {
  return {typeid(T).name(), sizeof(T), alignof(T), [] {
            return Object(std::make_unique<T>().release(), [](void *object) {
              std::default_delete<T>()(static_cast<T *>(object));
            });
          }};
}

// A registered command as help shows it: its words and placeholders, and what it does.
struct Usage {
  std::string_view usage;
  std::string_view help;
};

// Where every change goes, as text: a node sends commands as a person types them, so no
// node has a private way in.
class CommandPort {
public:
  // Runs a line and returns what its command answers. Sent during a frame, the line is a
  // group of its own, as a typed one, and a refusal goes to the log, naming its cause,
  // and answers nothing; sent by a command, see Call::commands.
  virtual std::string send(std::string_view line) = 0;
  // Every command registered now, in the order they registered (RV04). They last until
  // the next frame, which may register others.
  virtual std::vector<Usage> usages() const = 0;
  // What the log printed since the frame before, as the console shows it after the
  // time, so a node can show the log, refusals of the lines it sent included. It lasts
  // until the next frame.
  virtual std::span<const Logged> log() const = 0;
  // From now on the log, on the terminal and in what log hands on, keeps to the lines
  // about the nodes of these views, by the names a line addresses them with, and those of
  // the view vulpen started with; a line about no node, and every problem, stays (A02).
  // None keeps every line, as at the start. The part that shows the log sets it, so the
  // terminal vulpen was started from shows what a terminal in a window does.
  virtual void keep_log_to(std::span<const std::string> views) = 0;
  // The current view, where the lines a person types go, by its name, empty for the one
  // vulpen started with: where view switch moved it, and once that view is no longer
  // hosted, the nearest view that held it and still is. Every face shows it.
  virtual std::string_view current() = 0;

protected:
  ~CommandPort() = default;
};

// A key, text, the pointer, a button, the wheel or focus. The input command makes the
// same events without a window, so a headless test types and points as a person does.
struct Event {
  enum class Kind : std::uint8_t { key, text, pointer, button, wheel, focus };
  Kind kind = Kind::key;
  // A key or a button pressed, or let go; focus gained, or lost.
  bool down = false;
  // A key's name, as a, enter or f1; a button's, left, right or middle; the text typed.
  std::string name;
  glm::vec2 at{};   // where the pointer went, in pixels from the window's top left
  glm::vec2 turn{}; // how far the wheel turned
  // A button pressed: how many presses of it came in a row, each soon after the one
  // before and near it, so 2 is a double click's second; 0 for every other event.
  std::uint32_t presses = 0;

  bool operator==(const Event &) const = default;
};

// What the window or the input command made since the frame before, which every node
// reads alike.
class InputPort {
public:
  virtual std::span<const Event> events() const = 0;
  // Where the pointer is, as the frame block's cursor says: zero until it first moves.
  virtual glm::vec2 pointer() const = 0;
  // Whether a key event may carry the name, as the input command checks it, so a part
  // that binds keys refuses one no key has.
  virtual bool names_key(std::string_view name) const = 0;
  // The text the system's clipboard holds, or with no window what a node copied last,
  // so a run with none copies and pastes as one with a window does (V07).
  virtual std::string clipboard() const = 0;
  virtual void copy(std::string_view text) = 0;
  // Whether a window is open, whose events come here too, so the end of the terminal's
  // input ends no run a window still holds.
  virtual bool windowed() const = 0;

protected:
  ~InputPort() = default;
};

// Files a node reads, watches and saves. A save writes a temp file and renames it over
// the old one, so a killed run never leaves half a file. Each call throws naming the
// path it could not use.
class FilePort {
public:
  // What a file the node opened holds, read again once it changed on disk; empty while
  // it does not exist.
  virtual std::string_view text(File file) = 0;
  virtual void save(File file, std::string_view text) = 0;
  // Any other file, by absolute path, so the working directory never counts: a node
  // names one from its folder, and a command's <file> arguments come resolved.
  virtual std::string read(std::string_view file) = 0;
  // Makes the file's folder first when it has none.
  virtual void save(std::string_view file, std::string_view text) = 0;
  // Deletes a file, and its folder once nothing is left in it.
  virtual void remove(std::string_view file) = 0;
  // The names in a folder, sorted, a folder's ending in /; none when it does not exist.
  virtual std::vector<std::string> list(std::string_view folder) = 0;
  // A file, or a folder with all it holds, at a path where nothing is yet; whole or not
  // at all.
  virtual void copy(std::string_view from, std::string_view to) = 0;
  // A file or a folder moved to a path where nothing is yet.
  virtual void rename(std::string_view from, std::string_view to) = 0;
  // A view's manifest at another file, as view save writes it, keeping the comments the
  // file there holds, as a copy of the view's folder brings them.
  virtual void save(const View &view, std::string_view file) = 0;
  // A manifest's graph as a view runs it: each node with its folder and files, and in
  // the library each recipe a node uses unfolded into it (V11). Throws naming the
  // manifest's first mistake.
  virtual View manifest(std::string_view file) = 0;

protected:
  ~FilePort() = default;
};

// Programs a node starts and leaves running, as a person's own editor, which the graph
// never reads back.
class ProcessPort {
public:
  // Starts a program the PATH holds, each argument one word whatever it holds, and does
  // not wait for it. Throws naming a program the PATH lacks (A02).
  virtual void start(std::string_view program,
                     std::span<const std::string> arguments) = 0;

protected:
  ~ProcessPort() = default;
};

// Lines typed on standard input and text for standard output, so a CLI needs no window.
class TerminalPort {
public:
  // Whether a person types at the terminal, a terminal this run holds, not a pipe or a
  // file; then keys come one by one, and lines come from none.
  virtual bool typed() const = 0;
  // The keys a person typed since the last frame, named as a window names them, and the
  // text typed. The first ask takes the terminal's keys as they come, with no echo,
  // until the run ends, which gives the terminal back as it found it.
  virtual std::span<const Event> keys() = 0;
  // The lines that came in since the last frame, without their line breaks. Only a node
  // that asks makes the port read standard input, so a run nobody types to never reads
  // it, and a run in the background is never stopped for it.
  virtual std::span<const std::string> lines() = 0;
  // Once standard input has ended, as a piped script's does, and its lines were given.
  virtual bool ended() const = 0;
  // A line of standard output.
  virtual void print(std::string_view text) = 0;
  // Text where the next line is typed, with no line break. It shows only where a person
  // types, so a piped script's output stays as it was.
  virtual void prompt(std::string_view text) = 0;
  // The line being typed, in place of the one before, scrolled across to show the caret,
  // with the cursor there. What prints after, and the log, takes its place, so it is
  // drawn again.
  virtual void line(std::string_view text, std::size_t caret) = 0;
  // How many characters a row holds.
  virtual std::size_t columns() const = 0;

protected:
  ~TerminalPort() = default;
};

// What a node's C++ gets while it binds: names in, handles out. The loader checks each
// name against the node's shader and manifest entry before a frame runs (A02), so a
// frame looks nothing up.
class Bind {
public:
  template <class T> Value<T> value(std::string_view name) {
    static_assert(!glsl_type<T>.empty(),
                  "a pass block value is a float, int, uint or a glm vector of them");
    return {value_offset(name, glsl_type<T>)};
  }
  // An element is checked against the shader by its type, or a struct by its members.
  template <class T> Readback<T> readback(std::string_view name) {
    return {readback_index(name, element_of<T>())};
  }
  // One element per invocation of the node, or room for count when C++ writes more,
  // as a draw's instances; never fewer than one per invocation, so a shader that
  // indexes by its invocation stays inside the buffer (GLSL02).
  template <class T> Upload<T> upload(std::string_view name, std::uint32_t count = 0) {
    return {upload_index(name, element_of<T>(), count)};
  }
  // The image a Texture of the node's shaders names, or one that a connection takes to
  // another node's Texture; frame.upload fills it. T is checked against the format the
  // node's image word gives the port, R8G8B8A8_UNORM's u8vec4 when it gives none.
  template <class T> Texture<T> texture(std::string_view port) {
    static_assert(
        !pixel_type<T>.empty(),
        "a pixel is a uint8, uint16 or float, or a glm vector of 2 or 4 of them");
    return {texture_index(port, pixel_type<T>)};
  }
  // A number, read as the shader would read it, or a word as a std::string, such as a
  // file's name, which only C++ reads.
  template <class T> T param(std::string_view name) {
    const std::string_view text = param_text(name);
    if constexpr (std::is_same_v<T, std::string>) {
      return T(text);
    } else {
      T value{};
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size())
        param_invalid(name, glsl_type<T>);
      return value;
    }
  }
  // The object a connection carries from this node to C++ nodes: one per connection,
  // which the engine owns (A01) and keeps, contents included, while this node's module
  // stays. The reference lasts until the next bind.
  template <class T> T &output(std::string_view port) {
    return *static_cast<T *>(output_object(port, kind_of<T>()));
  }
  // The object the node that writes the connection hands this one: const, since only
  // the writer changes it, and written before this node cooks.
  template <class T> const T &input(std::string_view port) {
    return *static_cast<const T *>(input_object(port, kind_of<T>()));
  }
  // A command the node answers in its command hook, registered with its usage and help
  // (RV04). It lasts while the node runs: a rebuild registers it again, and a node that
  // goes, or stops in error, takes it along.
  virtual Command command(std::string_view usage, std::string_view help) = 0;
  // A file the node reads and saves: a relative path names one in the node's folder.
  // It lasts while the node runs; a rebuild that opens it again shares it, unread.
  virtual File file(std::string_view path) = 0;
  // The node's folder, where its own files are (RV08), as an absolute path: in its view,
  // or in the library for a recipe a node of the library uses.
  virtual std::string folder() const = 0;
  // Whether the node sets the param, for one the node may go without. Read then, it is
  // as any param, and one a node sets that its operator never asks about stays a
  // mistake (A02).
  virtual bool sets(std::string_view name) = 0;
  // The node's name, as a line to its view names it, so the node can send a command
  // about itself, as param set, which the log keeps and a replay repeats.
  virtual std::string name() const = 0;
  // The name of the view the node runs in, as a line addresses it with `<name>:`, empty
  // for the one vulpen started with, so a line the node sends about itself reaches its
  // own view, whichever view the line that ran its command addressed.
  virtual std::string view_name() const = 0;
  // The engine, for a node that includes its headers by hand: what the node makes with
  // it, it destroys, and nothing it borrows outlives the next bind (the runtime
  // boundary in docs/plans/native-cpp.md).
  virtual const Engine &engine() const = 0;

protected:
  ~Bind() = default;

private:
  virtual std::uint32_t value_offset(std::string_view name, std::string_view type) = 0;
  virtual std::uint32_t readback_index(std::string_view name, const Element &element) = 0;
  virtual std::uint32_t
  upload_index(std::string_view name, const Element &element, std::uint32_t count) = 0;
  virtual std::uint32_t texture_index(std::string_view port, std::string_view pixel) = 0;
  virtual std::string_view param_text(std::string_view name) = 0;
  virtual void param_invalid(std::string_view name, std::string_view type) = 0;
  virtual void *output_object(std::string_view port, const Kind &kind) = 0;
  virtual const void *input_object(std::string_view port, const Kind &kind) = 0;
};

// What a node costs a frame, in milliseconds: its operator's cook on the CPU, and its
// pass on the GPU, between two timestamps the CPU reads once the frame's fence passed
// (VK02), so a frame later. Each smoothed over about a second, so a person can read it,
// and as the last frame took it, so a spike shows (C03). Times differ from run to run,
// so only what a person reads may show them, never a node's output (C01).
struct Cost {
  std::string_view node; // as the view runs it, each recipe a node uses unfolded
  float cpu = 0.0f;
  float cpu_last = 0.0f;
  float gpu = 0.0f;
  float gpu_last = 0.0f;
  bool cooks = false; // it has an operator, which cooked
  bool runs = false;  // it has a pass, which the GPU runs
  bool timed = false; // the GPU timed the pass, as one without timestamps cannot
};

// What a node's C++ gets every frame, before the GPU runs the node's pass.
class Cook {
public:
  template <class T> void set(Value<T> value, T to) {
    std::memcpy(block().data() + value.offset, &to, sizeof to);
  }
  // Empty until the GPU has written the buffer once.
  template <class T> std::span<const T> read(Readback<T> readback) const {
    const std::span<const std::byte> bytes = readback_bytes(readback.index);
    return {reinterpret_cast<const T *>(bytes.data()), bytes.size() / sizeof(T)};
  }
  // Every element, zeroed when the buffer is made; what the CPU writes stays until it
  // writes again.
  template <class T> std::span<T> write(Upload<T> upload) {
    const std::span<std::byte> bytes = upload_bytes(upload.index, std::nullopt);
    return {reinterpret_cast<T *>(bytes.data()), bytes.size() / sizeof(T)};
  }
  // The first count elements, the buffer's used length from now on: a draw whose
  // instance_count names the buffer's port runs that many instances. Throws when
  // the buffer holds fewer.
  template <class T> std::span<T> write(Upload<T> upload, std::size_t count) {
    const std::span<std::byte> bytes = upload_bytes(upload.index, count);
    return {reinterpret_cast<T *>(bytes.data()), bytes.size() / sizeof(T)};
  }
  // Row by row from the image's first pixel, which uv 0 samples. The image keeps the
  // pixels through rebuilds while the node and the port keep their names and format, so
  // a node uploads once. Throws unless pixels holds size.x by size.y.
  template <class T>
  void upload(Texture<T> texture,
              std::type_identity_t<std::span<const T>> pixels,
              glm::uvec2 size) {
    upload_image(texture.index, std::as_bytes(pixels), size);
  }
  // Whether the image holds no pixels: before the first upload, and after image clear or
  // a change of its format. A node that fills its image once fills it again then.
  template <class T> bool empty(Texture<T> texture) const {
    return image_empty(texture.index);
  }
  virtual std::uint64_t index() const = 0;
  // The window's size in pixels, or without one the size --size gives, which the frame
  // block's resolution follows, so C++ lays out what shaders draw; zero without either.
  virtual glm::uvec2 resolution() const = 0;
  virtual void log(Level level, std::string_view text) const = 0;
  // Keeps quit from ending the run while the node holds work the end would lose, as a
  // file's changes: the node calls it each frame it does, naming what, and quit refuses
  // while it did so this frame or the one before, naming the node and what. The window's
  // close button asks as quit does; quit force ends the run anyway.
  virtual void hold(std::string_view what) = 0;
  virtual CommandPort &commands() = 0;
  virtual InputPort &input() = 0;
  virtual TerminalPort &terminal() = 0;
  virtual FilePort &files() = 0;
  // The view the node runs in, as this frame runs it: each recipe a node of the library
  // uses unfolded. Read it while the node cooks; a rebuild replaces it.
  virtual const View &view() const = 0;
  // Any view by its name, as a line addresses it, empty for the one vulpen started with,
  // as the changes so far left it: what a save would write; null when no view has the
  // name. A read changes nothing, so a node still changes a view only by commands (V06).
  // Read it while the node cooks; a change replaces it.
  virtual const View *view(std::string_view name) const = 0;
  // The same view as the next frame runs it, as view() reads the node's own: each node
  // with its folder and files, and each recipe a node of the library uses unfolded, its
  // own node naming it, so the graph shows the code a recipe brings. Null as above.
  virtual const View *unfolded(std::string_view name) const = 0;
  // What each node of a view costs, by the view's name as above, in the order the view
  // runs them; none for a view of no such name. Read it while the node cooks; a rebuild
  // replaces it.
  virtual std::span<const Cost> costs(std::string_view view) const = 0;

protected:
  ~Cook() = default;

private:
  virtual std::span<std::byte> block() = 0;
  virtual std::span<const std::byte> readback_bytes(std::uint32_t index) const = 0;
  // Every element without a count, and the buffer's used length from now on.
  virtual std::span<std::byte> upload_bytes(std::uint32_t index,
                                            std::optional<std::size_t> count) = 0;
  virtual void upload_image(std::uint32_t index,
                            std::span<const std::byte> pixels,
                            glm::uvec2 size) = 0;
  virtual bool image_empty(std::uint32_t index) const = 0;
};

// One run of a command the node registered: its arguments, the text it answers, and the
// commands it sends, which the log keeps in one group with it.
class Call {
public:
  // Which command runs, for a handler that registered several.
  virtual bool is(Command command) const = 0;
  // The words after the command's name, as many as its usage takes.
  virtual std::span<const std::string_view> arguments() const = 0;
  // What the command answers, which goes back to whoever sent the line.
  virtual void reply(std::string_view text) = 0;
  // Where the command sends lines: each joins its group in the log and addresses its
  // view unless it names one, and a refusal fails the command too, naming its cause.
  virtual CommandPort &commands() = 0;
  // The view the command addresses, as the changes so far left it: what a save would
  // write. Read it while the command runs; a later change replaces it.
  virtual const View &view() const = 0;
  // A view by the name a line addresses it with, `<name>:`, or empty for the one vulpen
  // started with, as `:` addresses it; null when no view has the name. So a command acts
  // at once on another view than the one it addresses, as on its own node's.
  virtual const View *view(std::string_view name) const = 0;
  // What each node of the view the command addresses costs, in the order it runs them.
  virtual std::span<const Cost> costs() const = 0;
  virtual FilePort &files() = 0;
  virtual ProcessPort &processes() = 0;

protected:
  ~Call() = default;
};

// What answers a command: an operator for the commands its node registered, and an
// engine module for its own. The command port runs every command the same way (V06).
class CommandHandler {
public:
  virtual void command(Call &call) = 0;

protected:
  ~CommandHandler() = default;
};

// A node's behaviour. State in its members lasts until its folder's module is swapped;
// bind runs at load and again after every swap. A key, a click or a typed line reaches
// a node as a command or as input cook reads, so no other hook is needed.
class Operator : public CommandHandler {
public:
  virtual ~Operator() = default;
  virtual void bind(Bind &) {}
  virtual void cook(Cook &) {}
  void command(Call &) override {}
};

// Where a folder's C++ registers its operators, by the names a manifest's operator word
// uses.
class Registry {
public:
  template <class T> void add(std::string_view name) {
    add(name, []() -> std::unique_ptr<Operator> { return std::make_unique<T>(); });
  }

protected:
  using Make = std::unique_ptr<Operator> (*)();

  ~Registry() = default;

private:
  virtual void add(std::string_view name, Make make) = 0;
};

} // namespace VP

// The one exported symbol of a folder's C++, which builds as one module (RV08), so one
// file of the folder registers its operators. The build names and exports it, so the
// same source loads as a dev module or links into a release binary
// (docs/plans/live-code.md).
#define VP_OPERATORS(registry)                                                           \
  extern "C" VP_MODULE_EXPORT void VP_MODULE_ENTRY(VP::Registry &registry)

// A member of the struct S, for the list its members() returns: its name, written once,
// its GLSL type and its offset.
#define VP_MEMBER(S, name) ::VP::member<decltype(S::name)>(#name, offsetof(S, name))
