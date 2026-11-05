#include "contracts/Item.h"
#include "contracts/Palette.h"
#include "contracts/Place.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Ini.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view menu_file = "menu.ini";
// Where the name of what a menu opened for goes in a line; first, with a colon, it names
// the view the line addresses, as a typed line does. {/} is the folder of what was
// pressed, so a line makes something inside a node or beside a file.
constexpr std::string_view subject = "{}";
constexpr std::string_view address_mark = "{}:";
constexpr std::string_view folder_mark = "{/}";
// How a path from the prompt's folder starts when it names a file there or the folder.
constexpr std::string_view here = "./";
// The words before what a menu opens for, and before the words the pointer came onto or
// pressed, as the hit part sends them.
constexpr std::string_view for_word = "for";
constexpr std::string_view point_word = "point";
constexpr std::string_view press_word = "press";
// The terminal's command a pressed line goes through.
constexpr std::string_view type_word = "type";
// The columns the menu component lays out, a word each.
constexpr std::size_t column_count = 3;
// Placeholders that stand for a few known words, which a column lists as the terminal's
// complete answers them; the person types any other, as a new name.
constexpr std::array<std::string_view, 4> listed{"<recipe>", "<node>", "<view>",
                                                 "<connection>"};
// The most words such a column lists; for more, it shows the placeholder, whose press
// lists them in the terminal, as Tab does, which scrolls as a menu does not.
constexpr std::size_t listed_most = 16;
// Between words, and between the lines an answer holds, a word each.
constexpr std::string_view blanks = " \t\r\n";
// What complete takes for a word not begun yet, and type for an empty one, which keeps
// the blank a line begun ends with.
constexpr std::string_view no_word = "\"\"";

std::vector<std::string> words_of(std::string_view text) {
  std::vector<std::string> words;
  for (std::size_t at = text.find_first_not_of(blanks); at != std::string_view::npos;) {
    const std::size_t end = std::min(text.find_first_of(blanks, at), text.size());
    words.emplace_back(text.substr(at, end - at));
    at = text.find_first_not_of(blanks, end);
  }
  return words;
}

std::string joined(std::span<const std::string> words) {
  std::string text;
  for (const std::string &word : words)
    text.append(text.empty() ? "" : " ").append(word);
  return text;
}

// A menu.ini line: the section it is under, a kind of thing pressed, its words as typed,
// and how many of them are a column each, those past {}: before the first that holds {}
// or a placeholder.
struct Line {
  std::string section;
  std::vector<std::string> words;
  std::size_t first = 0; // 1 past {}:
  std::size_t path = 0;

  std::span<const std::string> columns() const {
    return std::span(words).subspan(first, path);
  }
  // The word after the columns, as <recipe>; empty for none.
  std::string_view next() const {
    return first + path < words.size() ? std::string_view(words[first + path])
                                       : std::string_view();
  }
  bool lists() const {
    return std::ranges::find(listed, next()) != listed.end();
  }
  bool names_subject() const {
    return std::ranges::any_of(words, [](const std::string &word) {
      return word.find('{') != std::string::npos;
    });
  }
};

// A line a command registered now takes, past its {}:, and no more words a column than
// the menu shows, counting the one a placeholder it lists takes (A02).
Line line_of(std::string_view text, std::string section, std::span<const VP::Usage> usages) {
  Line line{.section = std::move(section), .words = words_of(text)};
  line.first = line.words.front() == address_mark ? 1 : 0;
  const std::span<const std::string> sent = std::span(line.words).subspan(line.first);
  if (sent.empty() || !registered(joined(sent), usages))
    throw std::runtime_error(std::format("no command takes {}", text));
  while (line.first + line.path < line.words.size() &&
         line.words[line.first + line.path].find_first_of("{<") == std::string::npos)
    ++line.path;
  if (line.path + (line.lists() ? 1 : 0) > column_count)
    throw std::runtime_error(
        std::format("{} takes more columns than the menu's {}", text, column_count));
  return line;
}

// Each line under its section, in the order they come, so a menu shows them so.
std::vector<Line> lines_of(std::string_view text, std::span<const VP::Usage> usages) {
  std::vector<Line> lines;
  std::string section;
  for (const Row &row : rows_of(text)) {
    try {
      if (row.text.starts_with('[')) {
        section = row.text.ends_with(']')
                      ? std::string(trim(row.text.substr(1, row.text.size() - 2)))
                      : std::string();
        if (section.empty())
          throw std::runtime_error("a section is a kind of thing pressed in brackets, as "
                                   "[node]");
        if (section == point_word || section == press_word || section == for_word)
          throw std::runtime_error(
              std::format("[{}] is a word the menu command takes itself", section));
        continue;
      }
      if (section.empty())
        throw std::runtime_error("a line comes under a section, as [node]");
      Line line = line_of(row.text, section, usages);
      if (std::ranges::any_of(lines, [&](const Line &given) {
            return given.section == line.section && given.words == line.words;
          }))
        throw std::runtime_error(std::format("[{}] gives {} twice", section, row.text));
      lines.push_back(std::move(line));
    } catch (const std::runtime_error &mistake) {
      throw std::runtime_error(
          std::format("{}:{}: {}", menu_file, row.line, mistake.what()));
    }
  }
  return lines;
}

// What a press or a line opens: the section of the kind of thing pressed, and those whose
// lines follow it, muted, as a background's lines for the whole project; where its first
// column shows, or the area's top left as the frame lays it out; and what it opens for, a
// name each, which {} names in its lines.
struct Opening {
  std::vector<std::string> sections;
  std::optional<glm::ivec2> corner;
  std::vector<std::string> subjects;
};

// A word a column shows: a word of the lines, or one of the few a placeholder stands for,
// as a recipe's name.
struct Entry {
  std::string word;
  bool value = false;
};

// A line a press hands the terminal, as its type takes it, and whether it is only begun,
// for the person to finish there.
struct Handing {
  std::string line;
  bool begun = false;
};

// The menus menu.ini gives, by the kind of thing a right press is on, each a line as the
// terminal takes it (V06), so a menu teaches the line it sends: its words a column each,
// the first column holding the first words, and the pointer on a word opening beside it
// the words that may follow it, as Tab lists them, and in place of a placeholder that
// stands for a few known words, as <recipe>, what the terminal's complete answers, or,
// past listed_most of them, the placeholder itself. The lines of the sections named after
// the first follow it, muted, as a background's for the whole project. A press on the
// word that ends a line hands the line to the terminal's type, as a key's line goes too:
// run there, as if typed, so the terminal shows it and its answer, or, when it wants a
// word, as <name>, begun up to it, the terminal's panel shown and the focus on it, for
// the person to finish, what may come there listed for a placeholder pressed; with
// several things pressed, a line runs once for each. A part that knows what was pressed
// opens the menu for it with menu <kind> [<section>...] for <name>..., as the graph and a
// tab do; while it shows it holds the pointer, so nothing under it takes a press, and the
// focus, so escape shuts it; a press anywhere but on a word shuts it too, giving the
// focus back. A key presses a whole line with no menu showing, menu press <kind>
// <word>... for <name>..., as the graph's Delete presses remove, so the key runs what the
// menu would, shown in the terminal as a press is. A change takes effect the next frame,
// as the focus does, so a press on a word reaches its hit before the words go. The file
// is read again once it changes; a mistake in it stops the node, naming the line (A02).
class Menus final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _menu = node.command(
        "menu [<value>...]",
        "opens the lines menu.ini gives a kind of thing, a word a column, and those of "
        "the sections after it, muted; with for and names, {} names each; point and a "
        "section's words open the words after them, and press hands the line they end to "
        "the terminal, run, or begun up to the word it wants; press, a section, a line's "
        "words, for and names runs that line for each, with no menu showing, as a key "
        "does; with none, shuts the menus");
    _name = node.name();
    _view = node.view_name();
    _line = node.param<std::string>("line");
    _shows = node.sets("shows") ? node.param<std::string>("shows") : std::string();
    _file = node.file(menu_file);
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _area = &node.input<VP_VIEW::Rect>("area");
    _columns = {&node.output<VP_VIEW::Items>("first"),
                &node.output<VP_VIEW::Items>("second"),
                &node.output<VP_VIEW::Items>("third")};
    _at = &node.output<VP_VIEW::Rect>("at");
    _place = &node.input<VP_VIEW::Place>("place");
    _written = false;
  }

  void cook(VP::Cook &frame) override {
    read(frame.files(), frame.commands());
    // A line opening it for something came in before this frame's pointer reached this
    // node, as the graph's right press sends it, so it opens where the frame left the
    // pointer.
    if (_next && !_next->subjects.empty())
      for (const VP::Event &event : frame.input().events())
        if (event.kind == VP::Event::Kind::pointer)
          _next->corner = glm::ivec2(event.at);
    const bool opening = _next.has_value();
    if (_next)
      open(frame, *std::exchange(_next, std::nullopt));
    else if (_shutting)
      shut(frame);
    _shutting = false;
    follow(frame, opening);
    *_at = _shown ? VP_VIEW::Rect{.offset = _corner, .extent = {1, 0}} : VP_VIEW::Rect{};
    if (!_written)
      write(frame.commands());
  }

  // As it changed, or for a line before the first frame, as a script's, once every
  // command it names has registered.
  void read(VP::FilePort &files, VP::CommandPort &commands) {
    const std::string_view text = files.text(_file);
    if (_read && text == *_read)
      return;
    _lines = lines_of(text, commands.usages());
    _read.emplace(text);
    _written = false;
  }

  // Shown at the corner given, its first column only, or none.
  void open(VP::Cook &frame, Opening opening) {
    if (!_shown) {
      _returning = _typed->focus;
      _begun = false;
      frame.commands().send(std::format("focus {}", _name));
    }
    _corner = opening.corner.value_or(_shown ? _corner : _area->offset);
    _shown = true;
    _sections = std::move(opening.sections);
    _subjects = std::move(opening.subjects);
    _group.clear();
    _path.clear();
    _written = false;
  }

  // Gives the focus back unless a line was begun, which keeps it on the terminal.
  void shut(VP::Cook &frame) {
    if (!_shown)
      return;
    if (!std::exchange(_begun, false))
      frame.commands().send(std::format("focus {}", _returning));
    _shown = false;
    _written = false;
  }

  // While it shows it holds the pointer, so every press reaches it, and its own hits too:
  // a press shuts it, as its hits send what a word does, which wins; escape shuts it. A
  // press in the frame it opens in shuts nothing, as it is the press another node opened
  // it for, as the graph's right press.
  void follow(VP::Cook &frame, bool opening) {
    if (_shown)
      frame.hold_pointer();
    for (const VP::Event &event : frame.input().events()) {
      if (event.kind == VP::Event::Kind::pointer)
        _pointer = event.at;
      else if (event.kind == VP::Event::Kind::button && event.down && _shown && !opening)
        _shutting = true;
    }
    for (const VP::Event &event : _typed->to(_name))
      _shutting = _shutting || (event.kind == VP::Event::Kind::key && event.down &&
                                event.name == "escape");
  }

  // The kind's lines, then those of the sections after it; with several things pressed,
  // of the kind's only those that name what was pressed and want no word more, as they
  // run once for each. Opened for nothing, a line that addresses what was pressed has
  // nothing to address.
  bool offered(const Line &line, std::string_view group) const {
    if (line.section != group)
      return false;
    if (_subjects.empty())
      return line.first == 0;
    if (group != _sections.front() || _subjects.size() < 2)
      return true;
    return line.names_subject() &&
           std::ranges::none_of(line.words, [](const std::string &word) {
             return word.find('<') != std::string::npos;
           });
  }

  // The words a column shows in a group after the words before it: the next words of the
  // lines whose columns start with them, each once, and where such a line's columns end
  // at a placeholder a column lists, what complete answers.
  std::vector<Entry> entries(VP::CommandPort &commands,
                             std::string_view group,
                             std::span<const std::string> before) const {
    std::vector<Entry> found;
    const auto add = [&](std::string word, bool value) {
      if (std::ranges::find(found, word, &Entry::word) == found.end())
        found.push_back({std::move(word), value});
    };
    for (const Line &line : _lines) {
      const std::span<const std::string> columns = line.columns();
      if (!offered(line, group) || columns.size() < before.size() ||
          !std::ranges::equal(columns.first(before.size()), before))
        continue;
      if (columns.size() > before.size()) {
        add(columns[before.size()], false);
      } else if (line.lists()) {
        std::vector<std::string> values = completed(commands, line);
        if (values.size() > listed_most)
          add(std::string(line.next()), true);
        else
          for (std::string &value : values)
            add(std::move(value), true);
      }
    }
    return found;
  }

  // What the terminal's complete answers for the word after a line's columns, as Tab
  // lists it, for the view lines go to; none where no terminal completes, as headless.
  static std::vector<std::string> completed(VP::CommandPort &commands, const Line &line) {
    const std::vector<VP::Usage> usages = commands.usages();
    if (!registered("complete", usages))
      return {};
    const std::string_view current = commands.current();
    const std::string answer = commands.send(std::format(
        "{}complete {} {}",
        current.empty() ? std::string() : std::string(current) + ": ",
        joined(line.columns()),
        no_word));
    std::vector<std::string> values;
    for (std::string &word : words_of(answer))
      if (!word.starts_with('<'))
        values.push_back(std::move(word));
    return values;
  }

  // Only as they change, so a frame allocates nothing for them (CPP10): the first column
  // each section's first words, the muted ones after, and each column after it the words
  // after the path's.
  void write(VP::CommandPort &commands) {
    _written = true;
    for (VP_VIEW::Items *const column : _columns)
      column->clear();
    if (!_shown)
      return;
    for (const std::string &group : _sections)
      for (const Entry &entry : entries(commands, group, {}))
        _columns[0]->push_back(item(group, {}, entry));
    for (std::size_t depth = 1; depth <= _path.size() && depth < column_count; ++depth) {
      const std::span<const std::string> before = std::span(_path).first(depth);
      for (const Entry &entry : entries(commands, _group, before))
        _columns[depth]->push_back(item(_group, before, entry));
    }
  }

  // A word's commands name its group and the words up to it, so a press reaches the line
  // it ends whatever the columns show by then.
  VP_VIEW::Item
  item(std::string_view group, std::span<const std::string> before, const Entry &entry) const {
    std::string words = std::format("{} {}", group, joined(before));
    words.append(before.empty() ? "" : " ").append(entry.word);
    const bool on_path = _group == group && before.size() < _path.size() &&
                         std::ranges::equal(before, std::span(_path).first(before.size())) &&
                         _path[before.size()] == entry.word;
    const bool muted = group != _sections.front();
    return {.label = entry.word,
            .command = std::format("menu {} {}", press_word, words),
            .current = on_path,
            .role = muted ? std::optional(VP_VIEW::role("muted")) : std::nullopt,
            .hover = entry.value ? std::string()
                                 : std::format("menu {} {}", point_word, words)};
  }

  // A line made from menu.ini's: a value chosen in place of the placeholder that lists it,
  // {} naming what was pressed, a path as the terminal takes it, and {/} its folder.
  // Whole, it is run; else it is begun up to the first word it still wants, as <name>, or
  // {} for nothing pressed, past the part of that word before it, as docs/ before <name>;
  // and for the placeholder chosen in place of a list too long, what may come there
  // listed. A path begun so loses its ./, as the person goes on typing a path the
  // terminal counts from its prompt's folder anyway: new <name>, new face/<name>. As the
  // terminal's type takes it, which a line begun ends with "" for the blank it ends with,
  // as a line's words lose it.
  Handing handed(VP::Call &call,
                 const Line &line,
                 const std::optional<std::string> &value,
                 std::string_view named) const {
    std::vector<std::string> words = line.words;
    if (value)
      words[line.first + line.path] = *value;
    const std::string folder = named.empty() ? std::string() : folder_of(call, named);
    const std::string pressed =
        named.find('/') == std::string_view::npos
            ? std::string(named)
            : typed(std::filesystem::absolute(std::filesystem::path(named))
                        .lexically_normal());
    std::string text;
    for (std::string word : words) {
      replace(word, subject, pressed);
      replace(word, folder_mark, folder);
      if (word.find('<') != std::string::npos && word.starts_with(here))
        word.erase(0, here.size());
      const std::size_t wanted = std::min(word.find('<'), word.find('{'));
      text.append(text.empty() ? "" : " ").append(word.substr(0, wanted));
      if (wanted != std::string::npos)
        return {.line = std::format("{} {} {}{}",
                                    type_word,
                                    value && value->starts_with('<') ? "list" : "begin",
                                    text,
                                    text.empty() || text.ends_with(' ') ? no_word : ""),
                .begun = true};
    }
    return {.line = std::format("{} {}", type_word, text)};
  }

  static void replace(std::string &word, std::string_view mark, std::string_view with) {
    for (std::size_t at = word.find(mark); !with.empty() && at != std::string::npos;
         at = word.find(mark, at + with.size()))
      word.replace(at, mark.size(), with);
  }

  // Where the terminal's lines count from, as its prompt shows: where the graph is, or
  // where vulpen started while it shows nothing.
  std::filesystem::path from() const {
    return _place->folder.empty() ? std::filesystem::current_path()
                                  : std::filesystem::path(_place->folder);
  }

  // A path as the terminal takes it, from there: ./ before a name there, which open would
  // take for a file the view holds by name, and . for the folder itself.
  std::string typed(const std::filesystem::path &path) const {
    const std::filesystem::path shown = path.lexically_proximate(from());
    const bool bare = shown != "." && !shown.has_parent_path();
    return (bare ? "." / shown : shown).generic_string();
  }

  // The folder of what was pressed, as a line takes it: a path, which holds a /, as the
  // graph names one from where vulpen started, ./ at the top, is a folder's own or names
  // the folder holding it; a name is a node's of the view lines go to, its name's path in
  // the view's folder (RV08), a hosted view's, or a folder there no node names yet. None
  // for none of them.
  std::string folder_of(VP::Call &call, std::string_view named) const {
    if (named.find('/') != std::string_view::npos) {
      const std::filesystem::path path =
          std::filesystem::absolute(std::filesystem::path(named)).lexically_normal();
      const std::filesystem::path bare = path.filename().empty() ? path.parent_path() : path;
      const std::vector<std::string> names = call.files().list(bare.parent_path().string());
      const bool folder =
          std::ranges::find(names, bare.filename().string() + '/') != names.end();
      return typed(folder ? bare : bare.parent_path());
    }
    const VP::View *const view = call.view(call.commands().current());
    const std::filesystem::path own =
        view ? std::filesystem::absolute(view->file.parent_path() / VP::path_of(named))
                   .lexically_normal()
             : std::filesystem::path();
    if (view && std::ranges::find(view->nodes, named, &VP::Node::name) != view->nodes.end())
      return typed(own);
    if (const VP::View *const hosted = call.view(named))
      return typed(std::filesystem::absolute(hosted->file.parent_path()));
    if (view) {
      const std::vector<std::string> names = call.files().list(own.parent_path().string());
      if (std::ranges::find(names, own.filename().string() + '/') != names.end())
        return typed(own);
    }
    return {};
  }

  // The words pressed end a line, or a value a column lists ends it; any other press opens
  // the words after them, as the pointer coming onto them does, and keeps the menus shown,
  // though the press reached them as any press does, which shuts them.
  void press(VP::Call &call, std::string group, std::vector<std::string> words) {
    for (const Line &line : _lines) {
      const std::span<const std::string> columns = line.columns();
      if (!offered(line, group) || columns.size() > words.size() ||
          !std::ranges::equal(columns, std::span(words).first(columns.size())))
        continue;
      const bool ends = columns.size() == words.size() && !line.lists();
      const bool valued = line.lists() && words.size() == columns.size() + 1;
      if (ends || valued)
        return hand(call, line, valued ? std::optional(words.back()) : std::nullopt);
    }
    _group = std::move(group);
    _path = std::move(words);
    _shutting = false;
    _written = false;
  }

  // Run once for each thing pressed, or begun, the terminal's panel shown and the focus
  // given to it, which the menus then keep there.
  void hand(VP::Call &call, const Line &line, const std::optional<std::string> &value) {
    _shutting = true;
    bool begun = false;
    const auto send = [&](std::string_view named) {
      const Handing handing = handed(call, line, value, named);
      call.commands().send(handing.line);
      begun = handing.begun;
    };
    if (_subjects.empty() || !line.names_subject())
      send({});
    else
      for (const std::string &named : _subjects)
        send(named);
    if (!begun)
      return;
    if (!_shows.empty())
      call.commands().send(std::format("{}: panel show {}", _view, _shows));
    call.commands().send(std::format("focus {}", _line));
    _begun = true;
  }

  // A section's line that names what it is for and wants no word more, by its words, run
  // for each name, as a press on it in a menu opened for them runs it.
  void press_for(VP::Call &call,
                 std::span<const std::string_view> words,
                 std::vector<std::string> names) {
    const auto line = std::ranges::find_if(_lines, [&](const Line &line) {
      return !words.empty() && line.section == words.front() &&
             std::ranges::equal(line.columns(), words.subspan(1)) &&
             line.names_subject() &&
             std::ranges::none_of(line.words, [](const std::string &word) {
               return word.find('<') != std::string::npos;
             });
    });
    if (line == _lines.end() || names.empty())
      throw std::runtime_error(std::format(
          "menu press takes a section, the words of one of its lines that names what it "
          "is for and wants no word more, for and names, as menu press file remove for "
          "./notes.txt; {} gives {}",
          menu_file,
          sections()));
    _subjects = std::move(names);
    hand(call, *line, std::nullopt);
  }

  void command(VP::Call &call) override {
    if (!call.is(_menu))
      return;
    read(call.files(), call.commands());
    const std::span<const std::string_view> words = call.arguments();
    if (words.empty()) {
      _shutting = true;
      return;
    }
    if (const auto named_for = std::ranges::find(words, for_word);
        words.front() == press_word && named_for != words.end())
      return press_for(
          call, {words.begin() + 1, named_for}, {named_for + 1, words.end()});
    if (words.front() == point_word || words.front() == press_word) {
      if (words.size() < 3 || !_shown)
        throw std::runtime_error(std::format(
            "menu {} takes a section and its words, while a menu shows", words.front()));
      std::vector<std::string> path(words.begin() + 2, words.end());
      if (words.front() == press_word)
        return press(call, std::string(words[1]), std::move(path));
      _group = words[1];
      _path = std::move(path);
      _written = false;
      return;
    }
    const auto named_for = std::ranges::find(words, for_word);
    std::vector<std::string> sections(words.begin(), named_for);
    for (const std::string &section : sections)
      if (std::ranges::none_of(_lines, [&](const Line &line) {
            return line.section == section;
          }))
        throw std::runtime_error(std::format(
            "no section of {} is named {}; it gives {}", menu_file, section, this->sections()));
    if (named_for == words.end()) {
      _next.emplace(Opening{.sections = std::move(sections)});
      return;
    }
    if (sections.empty() || named_for + 1 == words.end())
      throw std::runtime_error("menu takes sections, then for and the names of what it "
                               "opens for, or no for");
    _next.emplace(Opening{.sections = std::move(sections),
                          .corner = glm::ivec2(_pointer),
                          .subjects = {named_for + 1, words.end()}});
  }

  std::string sections() const {
    std::string sections;
    for (const Line &line : _lines)
      if (sections.find(std::format("[{}]", line.section)) == std::string::npos)
        sections += std::format("{}[{}]", sections.empty() ? "" : ", ", line.section);
    return sections;
  }

  VP::Command _menu;
  std::string _name;
  std::string _view; // the view it runs in, where the panel it shows is
  std::string _line; // the node whose line a begun one is typed into, as the terminal
  std::string _shows; // the panel holding it, or a node it holds; none for none
  VP::File _file;
  std::optional<std::string> _read; // the file the lines are from
  std::vector<Line> _lines;
  const VP_VIEW::Typed *_typed = nullptr;
  const VP_VIEW::Rect *_area = nullptr; // where a line opening it shows it, at its top left
  std::array<VP_VIEW::Items *, column_count> _columns{};
  VP_VIEW::Rect *_at = nullptr;
  const VP_VIEW::Place *_place = nullptr; // where the terminal's lines count from
  bool _shown = false;
  glm::ivec2 _corner{};              // where it was opened
  std::vector<std::string> _sections; // the kind it opened for, then those muted
  std::vector<std::string> _subjects; // the names {} stands for
  std::string _group;                // the section the words chosen are of
  std::vector<std::string> _path;    // the words chosen, a column each
  std::optional<Opening> _next;      // what a line opened, for the next frame
  bool _shutting = false;            // a press or escape, which shuts it the next frame
  bool _begun = false;               // a line was begun, which keeps the focus there
  std::string _returning;            // the node in focus as the menus opened
  glm::vec2 _pointer{};              // as the events so far left it
  bool _written = false;             // the columns are as the above say
};

} // namespace

VP_OPERATOR(Menus);
