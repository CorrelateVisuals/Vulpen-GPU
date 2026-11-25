#pragma once

#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <format>
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
  void *output_object(std::string_view port, const Kind &) override {
    throw std::logic_error(std::format("the bench's bind holds no output {}", port));
  }
  const void *input_object(std::string_view port, const Kind &) override {
    throw std::logic_error(std::format("the bench's bind holds no input {}", port));
  }

  View _view;
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

} // namespace VP::bench
