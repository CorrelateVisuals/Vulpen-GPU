#include "contracts/Item.h"
#include "runtime/Operator.h"

// Static, so each view's copy of this part keeps stb_image to itself, and copies in two
// views link into one release binary; the formats open hands it alone, 8 bits a channel,
// read from the bytes the file port reads, so it holds no code for the others, and none
// that opens a file.
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr int rgba = 4; // channels a pixel takes, as the picture's format holds them
// At the label's end, so a tab cut short, which loses what its label starts with, still
// says it is a preview.
constexpr std::string_view preview_label = "{} (preview)";

// The image file its file param names, decoded by stb_image into its picture, sRGB as a
// file holds its colors, for the node a connection takes the picture to: read again once
// it changes on disk, so an image edited elsewhere shows anew, a preview of it. Its tab
// names the file, a preview, and its close mark lets go of the file, as param unset, which
// clears the picture, so its pixels go with it (A03). Its switch param names the switch
// whose second half shows the picture, set to it as a file opens and back as it closes, so
// a preview takes the place of what the first half shows while it is open, and the person
// is shown the file opened. A file it cannot read or decode is named in the log.
class ImageFile final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string file = node.sets("file") ? node.param<std::string>("file") : "";
    _opened = std::exchange(_file, file) != file;
    _switch = node.param<std::string>("switch");
    _name = node.name();
    _view = node.view_name();
    _picture = node.texture<glm::u8vec4>("picture");
    _watched = _file.empty() ? VP::File{} : node.file(_file);
    _tabs = &node.output<VP_VIEW::Items>("tabs");
    _tabs->clear();
    if (!_file.empty())
      _tabs->push_back(
          {.label = std::format(preview_label, std::filesystem::path(_file).filename().string()),
           .current = true,
           .close = std::format("{}: param unset {} file", _view, _name)});
  }

  void cook(VP::Cook &frame) override {
    if (std::exchange(_opened, false)) {
      _decoded.reset();
      if (_file.empty())
        frame.commands().send(std::format("{}: image clear {}.picture", _view, _name));
      frame.commands().send(std::format(
          "{}: param set {} shown {}", _view, _switch, _file.empty() ? "first" : "second"));
    }
    if (_file.empty())
      return;
    const std::string_view bytes = frame.files().text(_watched);
    const std::size_t read = std::hash<std::string_view>{}(bytes);
    if (_decoded == read)
      return;
    _decoded = read; // decoded once, whether it decodes or not, until it changes
    try {
      upload(frame, bytes);
    } catch (const std::runtime_error &failure) {
      frame.log(VP::Level::error, failure.what());
    }
  }

  void upload(VP::Cook &frame, std::string_view bytes) {
    glm::ivec2 size{};
    int channels = 0;
    const std::unique_ptr<stbi_uc, void (*)(void *)> pixels(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                              static_cast<int>(bytes.size()),
                              &size.x,
                              &size.y,
                              &channels,
                              rgba),
        stbi_image_free);
    if (!pixels)
      throw std::runtime_error(
          std::format("{} is no image stb_image reads: {}", _file, stbi_failure_reason()));
    const auto count = static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y);
    frame.upload(_picture,
                 std::span(reinterpret_cast<const glm::u8vec4 *>(pixels.get()), count),
                 glm::uvec2(size));
  }

  std::string _file;   // the param's, as an absolute path; empty for none
  bool _opened = false; // another file, or none, since the frame before
  std::string _switch;
  std::string _name;
  std::string _view; // the one it runs in, where its lines go
  VP::Texture<glm::u8vec4> _picture;
  VP::File _watched;
  // The file's bytes the picture holds, by their hash; none until one decoded.
  std::optional<std::size_t> _decoded;
  VP_VIEW::Items *_tabs = nullptr;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<ImageFile>("ImageFile");
}
