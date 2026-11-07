#include "Lines.h"

#include "contracts/Item.h"
#include "contracts/Palette.h"
#include "contracts/Place.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Words.h"

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

// How a path from the prompt's folder starts when it names a file there or the folder.
constexpr std::string_view here = "./";
// The terminal's command a pressed line goes through.
constexpr std::string_view type_word = "type";
// The most words such a column lists; for more, it shows the placeholder, whose press
// lists them in the terminal, as Tab does, which scrolls as a menu does not.
constexpr std::size_t listed_most = 16;
// What complete takes for a word not begun yet, and type for an empty one, which keeps
// the blank a line begun ends with.
constexpr std::string_view no_word = "\"\"";
// The ports port's kind for a value a param sets, which no line connects.
constexpr std::string_view value_kind = "value";
constexpr std::string_view image_kind = "image";
// What a start of C++, and of each shader, ends with, as the files part names them.
constexpr std::string_view cpp_start = ".cpp";
constexpr std::string_view compute_start = ".comp";
constexpr std::string_view vertex_start = ".vert";
constexpr std::string_view fragment_start = ".frag";
constexpr std::array<std::string_view, 3> shader_ends{compute_start, vertex_start,
                                                       fragment_start};

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
// as a recipe's name, one with words after it, as a node's loose port; and whether a line
// joining several pressed gives it, which comes before those run once for each.
struct Entry {
  std::string word;
  bool value = false;
  bool opens = false;
  bool joins = false;
};

// A line a press hands the terminal, as its type takes it, and whether it is only begun,
// for the person to finish there.
struct Handing {
  std::string line;
  bool begun = false;
};

// A line joining two loose ports alike, as a column shows it: for one node its port and
// the other end, else the connection's name alone, and the line that makes it, its
// writer first.
struct Link {
  std::string first;
  std::string second;
  std::string line;
};

// What a column's words are found from, for the view lines go to, as the frame or the
// line sent reads it.
struct Look {
  const VP::View *view = nullptr;
  std::span<const VP::NodePort> ports;
  VP::FilePort &files;
};

// The menus menu.ini gives, by the kind of thing a right press is on, each a line as the
// terminal takes it (V06), so a menu teaches the line it sends: its words a column each,
// the first column holding the first words, and the pointer on a word opening beside it
// the words that may follow it, as Tab lists them, and in place of a placeholder that
// stands for a few known words, as <recipe>, what the terminal's complete answers, or,
// past listed_most of them, the placeholder itself; or, for what the menus find, what the
// view's ports, connections and files give, as <link>. The lines of the sections named
// after the first follow it, muted, as a background's for the whole project. A press on
// the word that ends a line hands the line to the terminal's type, as a key's line goes
// too: run there, as if typed, so the terminal shows it and its answer, or, when it wants
// a word, as <name>, begun up to it, the terminal's panel shown and the focus on it, for
// the person to finish, what may come there listed for a placeholder pressed. With
// several things pressed, a line joining them runs once, and under a rule after those, a
// line that wants no word typed runs once for each. A part that knows what was pressed
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
    if (!_written) {
      const std::string current(frame.commands().current());
      write(frame.commands(),
            Look{frame.view(current), frame.ports(current), frame.files()});
    }
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
    // An image of its own, as view show takes: a draw into the window has none, so its
    // port names no format.
    _image = false;
    if (_subjects.size() == 1)
      for (const VP::NodePort &port : frame.ports(frame.commands().current()))
        _image =
            _image || (port.kind == image_kind && port.writes && !port.holds.empty() &&
                       _subjects.front() == std::format("{}.{}", port.node, port.name));
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
  // of the kind's only those that join them, and those that name what was pressed and
  // want no word typed, as they run once for each, a value listed chosen once for all.
  // Opened for nothing, a line that addresses what was pressed has nothing to address.
  bool offered(const Line &line, std::string_view group) const {
    if (line.section != group || (line.names_image() && !_image))
      return false;
    if (_subjects.empty())
      return line.first == 0;
    if (group != _sections.front() || _subjects.size() < 2)
      return true;
    return line.joins() || (line.names_subject() && !line.wants() &&
                            (!line.lists() || one_of(completed_kinds, line.kind)));
  }

  // The words a column shows in a group after the words before it: the next words of the
  // lines whose columns start with them, each once, and where such a line's columns end
  // at a placeholder a column lists, what it stands for.
  std::vector<Entry> entries(VP::CommandPort &commands,
                             const Look &look,
                             std::string_view group,
                             std::span<const std::string> before) const {
    std::vector<Entry> found;
    const auto add = [&](Entry entry) {
      if (std::ranges::find(found, entry.word, &Entry::word) == found.end())
        found.push_back(std::move(entry));
    };
    for (const Line &line : _lines) {
      if (!offered(line, group) || (one_of(found_kinds, line.kind) &&
                                    values(commands, look, line, {}).empty()))
        continue;
      const std::span<const std::string> columns = line.columns();
      if (columns.size() > before.size()) {
        if (std::ranges::equal(columns.first(before.size()), before))
          add({.word = columns[before.size()], .joins = line.joins()});
      } else if (line.lists() &&
                 std::ranges::equal(columns, before.first(columns.size()))) {
        for (Entry &entry : values(commands, look, line, before.subspan(columns.size())))
          add(std::move(entry));
      }
    }
    return found;
  }

  // What a line's listed placeholder stands for, past the words chosen after its columns:
  // what the terminal's complete answers, as Tab lists it, or past listed_most of them
  // the placeholder; or what the menus find for what was pressed.
  std::vector<Entry> values(VP::CommandPort &commands,
                            const Look &look,
                            const Line &line,
                            std::span<const std::string> chosen) const {
    std::vector<Entry> entries;
    if (one_of(completed_kinds, line.kind)) {
      if (!chosen.empty())
        return entries;
      std::vector<std::string> words = completed(commands, line);
      if (words.size() > listed_most)
        return {{.word = line.kind, .value = true}};
      for (std::string &word : words)
        entries.push_back(
            {.word = std::move(word), .value = true, .joins = line.joins()});
      return entries;
    }
    if (line.kind == link_kind) {
      const bool two = two_columns(look);
      for (const Link &link : _links)
        if (chosen.empty() || (two && chosen.size() == 1 && link.first == chosen[0]))
          entries.push_back({.word = chosen.empty() ? link.first : link.second,
                             .value = true,
                             .opens = chosen.empty() && two,
                             .joins = true});
      std::ranges::sort(entries, {}, &Entry::word);
      const auto [kept, end] = std::ranges::unique(entries, {}, &Entry::word);
      entries.erase(kept, end);
      return entries;
    }
    if (!chosen.empty())
      return entries;
    for (std::string &word : found(look, line))
      entries.push_back(
          {.word = std::move(word), .value = true, .joins = line.joins()});
    return entries;
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

  // The node a name pressed is: a node's own, or for a port, as node.port, its node.
  static std::string node_of(const Look &look, std::string_view named) {
    const bool node = std::ranges::any_of(look.ports, [&](const VP::NodePort &port) {
      return port.node == named;
    });
    const std::size_t dot = named.rfind('.');
    return node || dot == std::string_view::npos ? std::string(named)
                                                 : std::string(named.substr(0, dot));
  }

  // Whether a link shows as a node's port and the other end, a column each: for one node
  // pressed, not a port of it.
  bool two_columns(const Look &look) const {
    return _subjects.size() == 1 && node_of(look, _subjects.front()) == _subjects.front();
  }

  // What the menus find for a placeholder but <link>: the connections joining what was
  // pressed, the values of a node pressed a param may set, the files in its folder, or
  // the files new starts that it lacks, as Name.cpp beside its shaders, or a lone
  // shader's other half.
  std::vector<std::string> found(const Look &look, const Line &line) const {
    std::vector<std::string> words;
    if (line.kind == joined_kind && look.view) {
      for (const VP::Connection &connection : look.view->connections)
        if (std::ranges::all_of(_subjects, [&](const std::string &named) {
              return joins(connection, named);
            }))
          words.push_back(connection.name);
      return words;
    }
    if (_subjects.size() != 1)
      return words;
    const std::string &named = _subjects.front();
    if (line.kind == key_kind) {
      for (const VP::NodePort &port : look.ports)
        if (port.kind == value_kind && port.node == named)
          words.emplace_back(port.name);
      return words;
    }
    const std::string folder = absolute_folder(look, node_of(look, named));
    std::vector<std::string> files;
    for (std::string &name : folder.empty() ? std::vector<std::string>()
                                            : look.files.list(folder))
      if (!name.ends_with('/') && !name.starts_with('.'))
        files.push_back(std::move(name));
    if (line.kind == path_kind)
      return files;
    const auto holds = [&](std::string_view end) {
      return std::ranges::any_of(
          files, [&](const std::string &file) { return file.ends_with(end); });
    };
    const std::string stem = class_of(named.substr(named.rfind('.') + 1));
    if (!holds(cpp_start))
      words.push_back(stem + std::string(cpp_start));
    if (holds(cpp_start) && std::ranges::none_of(shader_ends, holds))
      words.push_back(stem + std::string(compute_start));
    // A draw takes both halves: a lone one's other.
    if (holds(vertex_start) != holds(fragment_start))
      words.push_back(
          stem + std::string(holds(vertex_start) ? fragment_start : vertex_start));
    return words;
  }

  // Whether a connection's ends hold what was pressed: a node's, or a port's, node.port.
  static bool joins(const VP::Connection &connection, std::string_view named) {
    const auto at = [&](const VP::Endpoint &end) {
      return end.node == named || end.text() == named;
    };
    return at(connection.from) || std::ranges::any_of(connection.to, at);
  }

  // The lines a <link> stands for, for what was pressed: one node's loose ports and the
  // ports alike to each, a port's alike, or those joining two or more nodes pressed; each
  // connection named for its writer's port, or while one of the view holds that name, for
  // the writer's node and the port, as a bind's error names it.
  std::vector<Link> links_of(const Look &look) const {
    std::vector<Link> links;
    if (_subjects.empty())
      return links;
    const bool two = two_columns(look);
    const bool port = _subjects.size() == 1 && !two;
    const auto pressed = [&](std::string_view node) {
      return std::ranges::find(_subjects, node) != _subjects.end();
    };
    for (const VP::NodePort &own : look.ports) {
      if (own.joined || own.kind == value_kind)
        continue;
      const std::string name = std::format("{}.{}", own.node, own.name);
      for (const std::string &other : own.alike) {
        const std::string other_node = other.substr(0, other.rfind('.'));
        if (port ? name != _subjects.front()
                 : !pressed(own.node) || (!two && !pressed(other_node)))
          continue;
        const std::string &writer = own.writes ? name : other;
        const std::string &reader = own.writes ? other : name;
        const std::string line = joining(look, writer, reader);
        const std::string label = line.substr(line.find(' ') + 1,
                                              line.find(' ', line.find(' ') + 1) -
                                                  line.find(' ') - 1);
        if (std::ranges::none_of(
                links, [&](const Link &link) { return link.line == line; }))
          links.push_back({.first = two ? std::string(own.name) : port ? other : label,
                           .second = two ? other : std::string(),
                           .line = line});
      }
    }
    return links;
  }

  // As a bind's error names the line joining a writer to its reader.
  static std::string
  joining(const Look &look, std::string_view writer, std::string_view reader) {
    const std::size_t dot = writer.rfind('.');
    const std::size_t before =
        dot == 0 ? std::string_view::npos : writer.rfind('.', dot - 1);
    const std::size_t node = before == std::string_view::npos ? 0 : before + 1;
    std::string name(writer.substr(dot + 1));
    const auto taken = [&](const VP::Connection &connection) {
      return connection.name == name;
    };
    if (look.view && std::ranges::any_of(look.view->connections, taken))
      name = std::format("{}-{}", writer.substr(node, dot - node), name);
    return std::format("connect {} {} {}", name, writer, reader);
  }

  // Only as they change, so a frame allocates nothing for them (CPP10): the first column
  // each section's first words, those of lines joining several pressed first, then a
  // rule, then those run once for each, the muted ones after, and each column after it
  // the words after the path's.
  void write(VP::CommandPort &commands, const Look &look) {
    _written = true;
    for (VP_VIEW::Items *const column : _columns)
      column->clear();
    if (!_shown)
      return;
    _links = links_of(look);
    for (const std::string &group : _sections) {
      bool joining = false;
      for (const Entry &entry : entries(commands, look, group, {})) {
        if (joining && !entry.joins && _subjects.size() > 1)
          _columns[0]->push_back({});
        joining = entry.joins && _subjects.size() > 1;
        _columns[0]->push_back(item(group, {}, entry));
      }
    }
    for (std::size_t depth = 1; depth <= _path.size() && depth < column_count; ++depth) {
      const std::span<const std::string> before = std::span(_path).first(depth);
      for (const Entry &entry : entries(commands, look, _group, before))
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
            .hover = entry.value && !entry.opens
                         ? std::string()
                         : std::format("menu {} {}", point_word, words)};
  }

  // A line made from menu.ini's: a value chosen in place of the placeholder that lists it,
  // after what comes before it in its word, {} naming what was pressed, a path as the
  // terminal takes it, {/} its folder, {1} and {2} the first and second pressed, {node}
  // a port's node, and {view} the view lines go to. Whole, it is run; else it is begun up
  // to the first word it still wants, as <name>, or {} for nothing pressed, past the part
  // of that word before it, as docs/ before <name>; for the placeholder chosen in place
  // of a list too long, or a value that is a folder, going on into it as Tab does, what
  // may come there listed. A path begun so loses its ./, as the person goes on typing a
  // path the terminal counts from its prompt's folder anyway: new <name>, new
  // face/<name>. As the terminal's type takes it, which a line begun ends with "" for the
  // blank it ends with, as a line's words lose it.
  Handing handed(VP::Call &call,
                 const Line &line,
                 const std::optional<std::string> &value,
                 std::string_view named) const {
    std::vector<std::string> words = line.words;
    if (value)
      words[line.listed] = line.prefix + *value;
    const std::string folder = named.empty() ? std::string() : folder_of(call, named);
    const auto pressed = [&](std::string_view name) {
      return name.find('/') == std::string_view::npos
                 ? std::string(name)
                 : typed(std::filesystem::absolute(std::filesystem::path(name))
                             .lexically_normal());
    };
    const std::string one = _subjects.empty() ? std::string() : pressed(_subjects[0]);
    const std::string two = _subjects.size() < 2 ? std::string() : pressed(_subjects[1]);
    const std::string node =
        named.empty() ? std::string() : std::string(named.substr(0, named.rfind('.')));
    std::string text;
    for (std::size_t at = 0; at < words.size(); ++at) {
      std::string word = words[at];
      replace(word, subject, pressed(named));
      replace(word, folder_mark, folder);
      replace(word, first_mark, one);
      replace(word, second_mark, two);
      replace(word, node_mark, node);
      replace(word, view_mark, call.commands().current());
      replace(word, image_mark, named);
      if (word.find('<') != std::string::npos && word.starts_with(here))
        word.erase(0, here.size());
      const std::size_t wanted = std::min(word.find('<'), word.find('{'));
      text.append(text.empty() ? "" : " ").append(word.substr(0, wanted));
      if (value && at == line.listed && value->ends_with('/'))
        return {.line = std::format("{} list {}", type_word, text), .begun = true};
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

  // The folder of a node of the view lines go to, its name's path in the view's folder
  // (RV08), whole; none for a name no node of it has.
  static std::string absolute_folder(const Look &look, std::string_view named) {
    if (!look.view || look.view->file.empty() ||
        std::ranges::none_of(look.view->nodes,
                             [&](const VP::Node &node) { return node.name == named; }))
      return {};
    return std::filesystem::absolute(look.view->file.parent_path() / VP::path_of(named))
        .lexically_normal()
        .string();
  }

  // The folder of what was pressed, as a line takes it: a path, which holds a /, as the
  // graph names one from where vulpen started, ./ at the top, is a folder's own or names
  // the folder holding it; a name is a node's of the view lines go to, its name's path in
  // the view's folder (RV08), a port's node's, a hosted view's, or a folder there no
  // node names yet. None for none of them.
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
    // A port, as node.port, its node's.
    if (const std::size_t dot = named.rfind('.');
        view && dot != std::string_view::npos &&
        std::ranges::find(view->nodes, named.substr(0, dot), &VP::Node::name) !=
            view->nodes.end())
      return folder_of(call, named.substr(0, dot));
    if (const VP::View *const hosted = call.view(named))
      return typed(std::filesystem::absolute(hosted->file.parent_path()));
    if (view) {
      const std::vector<std::string> names = call.files().list(own.parent_path().string());
      if (std::ranges::find(names, own.filename().string() + '/') != names.end())
        return typed(own);
    }
    return {};
  }

  // The words pressed end a line, or a value a column lists ends it, as the second of a
  // node's link does; any other press opens the words after them, as the pointer coming
  // onto them does, and keeps the menus shown, though the press reached them as any press
  // does, which shuts them.
  void press(VP::Call &call, std::string group, std::vector<std::string> words) {
    for (const Line &line : _lines) {
      const std::span<const std::string> columns = line.columns();
      if (!offered(line, group) || columns.size() > words.size() ||
          !std::ranges::equal(columns, std::span(words).first(columns.size())))
        continue;
      const std::size_t past = words.size() - columns.size();
      if (line.kind == link_kind && past > 0)
        if (const auto link = std::ranges::find_if(_links, [&](const Link &link) {
              return link.first == words[columns.size()] &&
                     (past == 1 ? link.second.empty() : link.second == words.back());
            });
            link != _links.end())
          return hand_link(call, *link);
      const bool ends = past == 0 && !line.lists();
      const bool valued = line.lists() && line.kind != link_kind && past == 1;
      if (ends || valued)
        return hand(call, line, valued ? std::optional(words.back()) : std::nullopt);
    }
    _group = std::move(group);
    _path = std::move(words);
    _shutting = false;
    _written = false;
  }

  void hand_link(VP::Call &call, const Link &link) {
    _shutting = true;
    call.commands().send(std::format("{} {}", type_word, link.line));
  }

  // Run once for what was pressed, a line joining several, or once for each, or begun,
  // the terminal's panel shown and the focus given to it, which the menus then keep
  // there.
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
    else if (line.joins())
      send(_subjects.front());
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
             line.names_subject() && !line.lists() && !line.wants();
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
  bool _image = false; // the one pressed is an image its node writes, which {image} names
  std::vector<Link> _links;          // what a <link> stands for, as the columns show it
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
