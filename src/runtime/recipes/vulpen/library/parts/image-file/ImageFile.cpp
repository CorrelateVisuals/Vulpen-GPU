#include "contracts/Item.h"
#include "runtime/Operator.h"
#include "shared/Picture.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

// "But we all, with open face beholding as in a glass the glory of the Lord, are changed
// into the same image from glory to glory, even as by the Spirit of the Lord."
// 2 Corinthians 3:18

// At the label's end, so a tab cut short, which loses what its label starts with, still
// says it is a preview.
constexpr std::string_view preview_label = "{} (preview)";

// The image file its file param names, decoded by stb_image into its picture, sRGB as a
// file holds its colors, for the node a connection takes the picture to: read again once
// it changes on disk, so an image edited elsewhere shows anew, a preview of it. Its tab
// names the file, a preview, and its close mark lets go of the file, as param unset,
// which clears the picture, so its pixels go with it (A03). Its switch param names the
// switch whose second half shows the picture, set to it as a file opens and back as it
// closes, so a preview takes the place of what the first half shows while it is open, and
// the person is shown the file opened. A file it cannot read or decode is named in the
// log; one just opened in another's place clears the picture too, so no other file's
// pixels show under its name.
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
          {.label = std::format(preview_label,
                                std::filesystem::path(_file).filename().string()),
           .current = true,
           .close = std::format("{}: param unset {} file", _view, _name)});
  }

  void cook(VP::Cook &frame) override {
    const bool opened = std::exchange(_opened, false);
    if (opened) {
      _decoded.reset();
      if (_file.empty())
        frame.commands().send(std::format("{}: image clear {}.picture", _view, _name));
      frame.commands().send(std::format("{}: param set {} shown {}",
                                        _view,
                                        _switch,
                                        _file.empty() ? "first" : "second"));
    }
    if (_file.empty())
      return;
    const std::uint64_t read = frame.files().version(_watched);
    if (_decoded == read)
      return;
    _decoded = read; // decoded once, whether it decodes or not, until it changes
    try {
      const Pixels pixels = decoded(frame.files().text(_watched), _file);
      frame.upload(_picture, pixels.rgba, pixels.size);
    } catch (const std::runtime_error &failure) {
      frame.log(VP::Level::error, failure.what());
      if (opened)
        frame.commands().send(std::format("{}: image clear {}.picture", _view, _name));
    }
  }

  std::string _file;    // the param's, as an absolute path; empty for none
  bool _opened = false; // another file, or none, since the frame before
  std::string _switch;
  std::string _name;
  std::string _view; // the one it runs in, where its lines go
  VP::Texture<glm::u8vec4> _picture;
  VP::File _watched;
  // The file's version the picture holds; none until one decoded.
  std::optional<std::uint64_t> _decoded;
  VP_VIEW::Items *_tabs = nullptr;
};

} // namespace

VP_OPERATOR(ImageFile);
