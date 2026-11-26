#pragma once

#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The ports as recorders, for a case that runs code against a port as nodes do: each
// call a port takes is kept as text, which the case shows in its row, and each answer
// comes from what the case set. Only what a case calls is answered; the rest throw,
// naming the call, so a case never runs on an answer it did not choose (A02), or answer
// as a run with nothing of theirs would: NoViews hosts no view, and a ReplyCall's line
// has no arguments and addresses no view.

namespace VP::bench {

// A node's bind: its params by name, as a manifest sets them, and every name and type the
// node asked for.
class FakeBind final : public Bind {
public:
  std::vector<std::pair<std::string, std::string>> params;
  std::vector<std::string> asked;

  // A C++ connection's object by port, made when the node or the case first names it, and
  // kept across binds, as the engine keeps it: the case sets an input before a cook and
  // reads an output after.
  template <class T> T &object(std::string_view port) {
    return *static_cast<T *>(made(port, kind_of<T>()));
  }

  Command command(std::string_view usage, std::string_view help) override {
    asked.push_back(std::format("command {}: {}", usage, help));
    return {};
  }
  File file(std::string_view path) override {
    asked.push_back(std::format("file {}", path));
    return {};
  }
  std::string folder() const override {
    return "/node";
  }
  bool sets(std::string_view name) override {
    return std::ranges::any_of(params,
                               [&](const auto &param) { return param.first == name; });
  }
  std::string name() const override {
    return "n";
  }
  std::string view_name() const override {
    return {};
  }
  const Engine &engine() const override {
    throw std::logic_error("the bench's bind holds no engine");
  }
  const View &view() const override {
    return _view;
  }

private:
  std::uint32_t value_offset(std::string_view name, std::string_view type) override {
    asked.push_back(std::format("value {} as {}", name, type));
    return 0;
  }
  std::uint32_t readback_index(std::string_view name, const Element &element) override {
    asked.push_back(
        std::format("readback {} of {} bytes {}", name, element.size, element.type));
    return 0;
  }
  std::uint32_t upload_index(std::string_view name,
                             const Element &element,
                             std::uint32_t count) override {
    asked.push_back(std::format(
        "upload {} of {} bytes {}, room {}", name, element.size, element.type, count));
    return 0;
  }
  std::uint32_t texture_index(std::string_view port, std::string_view pixel) override {
    asked.push_back(std::format("texture {} of {}", port, pixel));
    return 0;
  }
  std::string_view param_text(std::string_view name) override {
    for (const auto &[key, text] : params)
      if (key == name)
        return text;
    return {};
  }
  void param_read_as(std::string_view name, std::string_view type, bool valid) override {
    asked.push_back(std::format("param {} read as {}{}",
                                name,
                                type.empty() ? "a word" : type,
                                valid ? "" : ", refused"));
  }
  void *output_object(std::string_view port, const Kind &kind) override {
    asked.push_back(std::format("output {}", port));
    return made(port, kind);
  }
  const void *input_object(std::string_view port, const Kind &kind) override {
    asked.push_back(std::format("input {}", port));
    return made(port, kind);
  }
  // The type is compared by its name, as typeid gives it, which only the message would
  // show, and differs by compiler.
  void *made(std::string_view port, const Kind &kind) {
    auto found = _objects.find(port);
    if (found == _objects.end())
      found = _objects.emplace(std::string(port), std::pair{kind.type, kind.make()}).first;
    else if (std::string_view(found->second.first) != kind.type)
      throw std::logic_error(std::format("port {} holds an object of another type", port));
    return found->second.second.get();
  }

  View _view;
  std::map<std::string, std::pair<const char *, Object>, std::less<>> _objects;
};

// A node's cook: the frame's index, size and clock as the case sets them, every line the
// node logs, and the file port the case hands it.
class FakeCook final : public Cook {
public:
  std::uint64_t frame = 0;
  glm::uvec2 size{};
  double clock = 0;
  FilePort *file_port = nullptr;
  mutable std::vector<std::string> said;

  std::uint64_t index() const override {
    return frame;
  }
  glm::uvec2 resolution() const override {
    return size;
  }
  void log(Level level, std::string_view text) const override {
    said.push_back(std::format("{}: {}", level_names[static_cast<std::size_t>(level)], text));
  }
  double seconds() const override {
    return clock;
  }
  FilePort &files() override {
    if (!file_port)
      throw std::logic_error("the bench's cook hands no file port");
    return *file_port;
  }
  void hold(std::string_view) override {
    refused("hold");
  }
  CommandPort &commands() override {
    refused("commands");
  }
  InputPort &input() override {
    refused("input");
  }
  TerminalPort &terminal() override {
    refused("terminal");
  }
  SoundPort &sound() override {
    refused("sound");
  }
  const View &view() const override {
    refused("view");
  }
  const View *view(std::string_view) const override {
    refused("view of a name");
  }
  const View *unfolded(std::string_view) const override {
    refused("unfolded");
  }
  std::span<const Cost> costs(std::string_view) const override {
    refused("costs");
  }
  void hold_pointer() override {
    refused("hold_pointer");
  }
  std::span<const NodePort> ports(std::string_view) const override {
    refused("ports");
  }
  void hand_pointer(std::string_view, glm::vec2, glm::vec2) override {
    refused("hand_pointer");
  }
  void unread(std::string_view) override {
    refused("unread");
  }

private:
  [[noreturn]] static void refused(std::string_view call) {
    throw std::logic_error(std::format("the bench's cook answers no {}", call));
  }
  std::span<std::byte> block() override {
    refused("pass block");
  }
  std::span<const std::byte> readback_bytes(std::uint32_t) const override {
    refused("readback");
  }
  std::span<std::byte> upload_bytes(std::uint32_t, std::optional<std::size_t>) override {
    refused("upload");
  }
  void upload_image(std::uint32_t, std::span<const std::byte>, glm::uvec2) override {
    refused("image upload");
  }
  bool image_empty(std::uint32_t) const override {
    refused("image");
  }
  void upload_levels(std::uint32_t,
                     std::span<const std::byte>,
                     glm::uvec2,
                     std::uint32_t) override {
    refused("image upload");
  }
};

// The file port over the files themselves, by absolute path, as Ports gives a line them:
// each call through the engine's own Files, so what a step changes is on disk.
class SandboxFiles final : public FilePort {
public:
  std::string_view text(File) override {
    throw std::logic_error("the bench's files open no node's file");
  }
  void save(File, std::string_view) override {
    throw std::logic_error("the bench's files open no node's file");
  }
  std::string read(std::string_view file) override {
    return Files::text(file);
  }
  void save(std::string_view file, std::string_view text) override {
    Files::save(file, text);
  }
  void remove(std::string_view file) override {
    Files::remove(file);
  }
  std::vector<std::string> list(std::string_view folder) override {
    std::vector<std::string> names;
    std::error_code gone;
    for (const auto &entry : std::filesystem::directory_iterator(folder, gone))
      names.push_back(entry.path().filename().string() +
                      (entry.is_directory(gone) ? "/" : ""));
    std::ranges::sort(names);
    return names;
  }
  void copy(std::string_view from, std::string_view to) override {
    Files::copy(from, to);
  }
  void rename(std::string_view from, std::string_view to) override {
    Files::rename(from, to);
  }
  void save(const View &, std::string_view) override {
    throw std::logic_error("the bench's files save no view");
  }
  View manifest(std::string_view) override {
    throw std::logic_error("the bench's files read no manifest");
  }
  void make(std::string_view folder) override {
    Files::make(folder);
  }
  std::span<const Moved> moved() const override {
    return {};
  }
  std::uint64_t version(File) override {
    throw std::logic_error("the bench's files open no node's file");
  }
};

class NoProcesses final : public ProcessPort {
public:
  void start(std::string_view program, std::span<const std::string>) override {
    throw std::logic_error(std::format("the bench starts no program, as {}", program));
  }
};

// Views as none are hosted: a step changes files alone.
class NoViews final : public ViewLookup {
public:
  explicit NoViews(FilePort &files) : _files(files) {}
  const View *find(std::string_view) override {
    return nullptr;
  }
  const View *unfolded(std::string_view) override {
    return nullptr;
  }
  std::span<const Cost> costs(std::string_view) override {
    return {};
  }
  std::span<const NodePort> ports(std::string_view) override {
    return {};
  }
  void write(std::string_view) override {}
  std::vector<std::string> names() override {
    return {};
  }
  std::vector<std::string> unkept(std::string_view) override {
    return {};
  }
  void put_back(std::string_view) override {}
  void replace(std::string_view, View) override {}
  std::optional<std::string> host_of(std::string_view) override {
    return std::nullopt;
  }
  void check(std::string_view, const View &) override {}
  void begin_step(std::string_view) override {}
  void end_step(std::optional<std::size_t>) override {}
  FilePort &files() override {
    return _files;
  }
  std::size_t done() const override {
    return 0;
  }
  bool played() override {
    return true; // as Views answers with none hosted: no app among them
  }

private:
  FilePort &_files;
};

// A command's call that keeps what it answers; nothing else is asked of it.
class ReplyCall final : public Call {
public:
  std::vector<std::string> replies;

  bool is(Command) const override {
    return false;
  }
  std::span<const std::string_view> arguments() const override {
    return {};
  }
  void reply(std::string_view text) override {
    replies.emplace_back(text);
  }
  CommandPort &commands() override {
    throw std::logic_error("the bench's call sends no line");
  }
  const View &view() const override {
    throw std::logic_error("the bench's call addresses no view");
  }
  const View *view(std::string_view) const override {
    return nullptr;
  }
  std::span<const Cost> costs() const override {
    return {};
  }
  FilePort &files() override {
    throw std::logic_error("the bench's call reaches no files");
  }
  ProcessPort &processes() override {
    throw std::logic_error("the bench's call starts no program");
  }
  const View &unfolded() const override {
    throw std::logic_error("the bench's call addresses no view");
  }
  std::string resolved(std::string_view path) const override {
    return std::string(path);
  }
  std::span<const NodePort> ports() const override {
    return {};
  }
  void keep_name(std::size_t, std::string_view) override {}
};

// A part's operator as its node runs it: bound by the fake bind with the params given,
// then cooked by the fake cook; between, a case sets its inputs and frame, and after,
// reads its outputs.
template <class Part> class Running {
public:
  FakeBind bind;
  FakeCook cook;

  explicit Running(std::vector<std::pair<std::string, std::string>> params = {}) {
    bind.params = std::move(params);
  }
  void bound() {
    static_cast<Operator &>(_part).bind(bind);
  }
  void cooked() {
    static_cast<Operator &>(_part).cook(cook);
  }
  template <class T> T &port(std::string_view name) {
    return bind.object<T>(name);
  }

private:
  Part _part;
};

} // namespace VP::bench
