#include "runtime/Edits.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <string>
#include <string_view>
#include <vector>

// runtime/Edits.cpp: the primitive edits, the manifest's own words. Each edit runs on a
// copy of a world, as Edits::command runs it on a copy of the view, and its row shows
// what it changed. Its words keep to the count its usage gives, as the command port
// counts them before an edit runs; an empty word stands for one left out.

namespace VP::bench {

namespace domain {
// A node's or a connection's name as a line gives it: the worlds' own, one no world
// holds, and what no name may be.
struct NodeName {};
// A word=value a node takes, or refuses.
struct NodeWord {};
// A port, node.port, or a hosted view's window, view:.
struct Port {};
struct ParamKey {};
struct ParamValue {};
// A node word's key and value, as a manifest line gives them.
struct NodeKey {};
struct NodeValue {};
// A whole connect line's words, name, from and to, as the worlds' connections and ports
// meet them: a connection named again, a reader another takes, a window.
struct ConnectLine {};
} // namespace domain

template <> struct Gen<domain::ConnectLine> {
  using type = std::vector<std::string>;
  static std::vector<type> edges() {
    return {{"new", "a.out", "b.in"},
            {"ab", "a.out", "b.in"},
            {"ab", "a.out", "c.in"},
            {"ab", "a.out", "a.in"},
            {"bc", "a.out", "c.in"},
            {"ab", "b.out", "c.in"},
            {"rects", "ui.panel.rects", "b.in"},
            {"rects", "ui.panel.rects", "b.rects"},
            {"window", "game:", "show.picture"},
            {"window", "game:", "show.other"},
            {"window", "game:", "zz.in"},
            {"x", "game:", "show.picture"},
            {"y", "beside:", "show.p"},
            {"z", "zz.out", "a.in"}};
  }
  static type random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::NodeName> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"a",     "b",      "c",          "ui",   "ui.panel",  "ui.panel.text",
            "probe", "tool",   "tool.light", "show", "game",      "ab",
            "rects", "window", "zz",         "root", "contracts", "shared",
            "",      "a b",    "a..b",       ".a",   "a.",        "#",
            "1a"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::NodeWord> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"param=x=1",
            "",
            "param=x=",
            "param=x",
            "param=",
            "param=a.b=2",
            "param=x=1#c",
            "invocations=4",
            "invocations=0",
            "invocations=port",
            "invocations=4x",
            "invocations=99999999999",
            "vertex_count=3",
            "vertex_count=0",
            "instance_count=2",
            "instance_count=0",
            "instance_count=n",
            "recipe=lamp",
            "recipe=lamp@3f2a",
            "recipe=lamp@",
            "recipe=Lamp@3F",
            "log=info",
            "log=loud",
            "operator=Op",
            "image=atlas=R8_UNORM",
            "image=atlas=",
            "image==R8",
            "file=x",
            "x",
            "=",
            "invocations=",
            "log="};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::Port> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"a.out",
            "b.in",
            "c.in",
            "game:",
            "show.picture",
            "zz.in",
            "ui.panel.rects",
            "a",
            "a.",
            ".in",
            "",
            "x:y",
            "a.b.c",
            "tool.light.out",
            "b.out",
            "a.in"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::ParamKey> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"x", "rows", "", "a.b", "a b", "#", "tint", "light.tint"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::ParamValue> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"1", "", "1,2,3", "#", "a\nb", "a b", "x=y"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::NodeKey> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"param",
            "image",
            "recipe",
            "operator",
            "invocations",
            "vertex_count",
            "instance_count",
            "log",
            "file",
            "",
            "x",
            "Param"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::NodeValue> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"1",
            "",
            "x=1",
            "x",
            "lamp",
            "lamp@3f2a",
            "info",
            "atlas=R8_UNORM",
            "0",
            "port",
            "#x",
            "a\nb",
            "99999999999",
            "4x",
            "=",
            "x="};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

namespace {

using Edit = void (*)(View &, Arguments, const Around &);

// The views hosted beside the view, as Edits::command gives an edit.
std::vector<std::string> beside() {
  return {"beside"};
}

std::vector<std::string_view> words_of(std::initializer_list<std::string_view> given,
                                       std::size_t least) {
  std::vector<std::string_view> words;
  for (const std::string_view word : given)
    if (words.size() < least || !word.empty())
      words.push_back(word);
  return words;
}

// The edit on a copy of the view: what it changed.
std::string
edited(Edit edit, const View &view, const std::vector<std::string_view> &words) {
  View copy = view;
  const std::vector<std::string> others = beside();
  edit(copy, words, {.where = "case:1", .beside = others});
  return worlds::changes(view, copy);
}

// Refused, it leaves the view as it was, as Edits' comment says of every edit (A02).
bool whole_or_none(Edit edit,
                   const View &view,
                   const std::vector<std::string_view> &words) {
  View copy = view;
  const std::vector<std::string> others = beside();
  try {
    edit(copy, words, {.where = "case:1", .beside = others});
    return true;
  } catch (const std::runtime_error &) {
    return worlds::whole(copy) == worlds::whole(view);
  }
}

constexpr std::string_view as_it_was = "refused, it leaves the view as it was";

} // namespace

void cases(Bench &bench) {
  // Each edit, at the words its usage takes: node add <node> [<word=value>], node remove
  // <node>, node move <node> <node>, node set <node> <word=value>, connect <name> <port>
  // <port>, disconnect <name>, param set <node> <key> <value>, param unset <node> <key>,
  // view add <name> <file>, view close <name>.
  const auto one_word = [&](const std::string &id, Edit edit) {
    bench.sweep(
        id,
        [edit](const View &view, std::string_view name) {
          return edited(edit, view, {name});
        },
        domain::Type{},
        domain::NodeName{});
    bench.holds(
        id,
        std::string(as_it_was),
        [edit](const View &view, std::string_view name) {
          return whole_or_none(edit, view, {name});
        },
        domain::Type{},
        domain::NodeName{});
  };
  one_word("Edits.cpp/node_remove", node_remove);
  one_word("Edits.cpp/disconnect", disconnect);
  one_word("Edits.cpp/view_close", view_close);

  const auto with_word = [&](const std::string &id, Edit edit, std::size_t least) {
    bench.sweep(
        id,
        [edit, least](const View &view, std::string_view name, std::string_view word) {
          return edited(edit, view, words_of({name, word}, least));
        },
        domain::Type{},
        domain::NodeName{},
        domain::NodeWord{});
    bench.holds(
        id,
        std::string(as_it_was),
        [edit, least](const View &view, std::string_view name, std::string_view word) {
          return whole_or_none(edit, view, words_of({name, word}, least));
        },
        domain::Type{},
        domain::NodeName{},
        domain::NodeWord{});
  };
  with_word("Edits.cpp/node_add", node_add, 1);
  with_word("Edits.cpp/node_set", node_set, 2);

  bench.sweep(
      "Edits.cpp/node_move",
      [](const View &view, std::string_view name, std::string_view before) {
        return edited(node_move, view, {name, before});
      },
      domain::Type{},
      domain::NodeName{},
      domain::NodeName{});
  bench.holds(
      "Edits.cpp/node_move",
      std::string(as_it_was),
      [](const View &view, std::string_view name, std::string_view before) {
        return whole_or_none(node_move, view, {name, before});
      },
      domain::Type{},
      domain::NodeName{},
      domain::NodeName{});

  bench.sweep(
      "Edits.cpp/connect_ports",
      [](const View &view,
         std::string_view name,
         std::string_view from,
         std::string_view to) { return edited(connect_ports, view, {name, from, to}); },
      domain::Type{},
      domain::NodeName{},
      domain::Port{},
      domain::Port{});
  bench.holds(
      "Edits.cpp/connect_ports",
      std::string(as_it_was),
      [](const View &view, const std::vector<std::string> &line) {
        return whole_or_none(connect_ports, view, {line[0], line[1], line[2]});
      },
      domain::Type{},
      domain::ConnectLine{});

  // A connection named again from its own port takes the readers given beside its own;
  // these lines name the chain's ab, from a.out, with readers it may and may not take.
  bench.check("Edits.cpp/connect_ports again", [](Rows &rows) {
    const View chain = worlds::small()[2];
    const std::vector<std::string> others = beside();
    for (const std::string_view reader : {"b.in", "a.in", "c.in", "game:", "zz.in"}) {
      View copy = chain;
      std::string said;
      try {
        connect_ports(copy,
                      std::vector<std::string_view>{"ab", "a.out", reader},
                      {.where = "case:1", .beside = others});
        said = worlds::changes(chain, copy);
      } catch (const std::runtime_error &refused) {
        said = std::format("throws {}; the view after: {}",
                           escaped(refused.what()),
                           worlds::changes(chain, copy));
      }
      rows.row(std::format("chain, connect ab a.out {} -> {}", reader, said));
    }
  });

  bench.sweep(
      "Edits.cpp/param_set",
      [](const View &view,
         std::string_view node,
         std::string_view key,
         std::string_view value) { return edited(param_set, view, {node, key, value}); },
      domain::Type{},
      domain::NodeName{},
      domain::ParamKey{},
      domain::ParamValue{});
  bench.holds(
      "Edits.cpp/param_set",
      std::string(as_it_was),
      [](const View &view,
         std::string_view node,
         std::string_view key,
         std::string_view value) {
        return whole_or_none(param_set, view, {node, key, value});
      },
      domain::Type{},
      domain::NodeName{},
      domain::ParamKey{},
      domain::ParamValue{});

  bench.sweep(
      "Edits.cpp/param_unset",
      [](const View &view, std::string_view node, std::string_view key) {
        return edited(param_unset, view, {node, key});
      },
      domain::Type{},
      domain::NodeName{},
      domain::ParamKey{});
  bench.holds(
      "Edits.cpp/param_unset",
      std::string(as_it_was),
      [](const View &view, std::string_view node, std::string_view key) {
        return whole_or_none(param_unset, view, {node, key});
      },
      domain::Type{},
      domain::NodeName{},
      domain::ParamKey{});

  bench.sweep(
      "Edits.cpp/view_add",
      [](const View &view, std::string_view name) {
        return edited(view_add, view, {name, "/views/other/view.vlp"});
      },
      domain::Type{},
      domain::NodeName{});

  // The words and names every edit checks with.
  bench.sweep("Edits.cpp/whole_number", whole_number, domain::Word{});
  bench.sweep("Edits.cpp/is_recipe", is_recipe, domain::NodeWord{});
  bench.sweep("Edits.cpp/key_and_value", key_and_value, domain::NodeWord{});
  bench.sweep(
      "Edits.cpp/check_value", check_value, domain::ParamKey{}, domain::ParamValue{});
  bench.sweep("Edits.cpp/Edits::endpoint", Edits::endpoint, domain::Port{});
  bench.sweep(
      "Edits.cpp/Edits::word",
      [](std::string_view key, std::string_view value) {
        Node node{.name = "n"};
        Edits::word(node, key, value);
        Names names;
        return show(node, names);
      },
      domain::NodeKey{},
      domain::NodeValue{});
  bench.sweep(
      "Edits.cpp/reaches",
      [](const View &view, std::string_view writer, std::string_view reader) {
        return reaches(view, writer, reader);
      },
      domain::Type{},
      domain::NodeName{},
      domain::NodeName{});

  // A drop's copy names its recipe with an @ and the fingerprint of what it copied, in
  // lowercase hex: each end of both digit ranges, a character just past each, an
  // uppercase digit, and none.
  bench.check("Edits.cpp/is_recipe, a fingerprint's digits", [](Rows &rows) {
    for (const std::string_view text : {"lamp@0",
                                        "lamp@9",
                                        "lamp@a",
                                        "lamp@f",
                                        "lamp@/",
                                        "lamp@:",
                                        "lamp@`",
                                        "lamp@g",
                                        "lamp@A",
                                        "lamp@",
                                        "lamp@0123456789abcdef",
                                        "lamp"})
      rows.row(std::format("{} -> {}", text, is_recipe(text)));
  });
  // Words typed with blanks around an =, refused naming the word meant: a lone = with
  // its value after it, a value glued to the =, a lone = last, after a word=value, and
  // as the first word.
  bench.check("Edits.cpp/check_spaced", [](Rows &rows) {
    const std::vector<std::vector<std::string_view>> lines{{"vertex_count", "=", "6"},
                                                           {"vertex_count", "=6"},
                                                           {"vertex_count", "="},
                                                           {"a=1", "b", "=6"},
                                                           {"a=1", "b", "=", "6"},
                                                           {"a", "b"},
                                                           {"a=1", "=2"},
                                                           {"=", "6"}};
    for (const std::vector<std::string_view> &words : lines) {
      std::string said = "passes";
      try {
        check_spaced(words);
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("[{}] -> {}", joined(words, " "), said));
    }
  });
  // What a node set line answers: each word it named as the node holds it now, each key
  // once, all of a key given more than once (a node's params), and a key it holds no
  // longer as cleared.
  bench.check("Edits.cpp/named_words", [](Rows &rows) {
    View view;
    const std::vector<std::string> others = beside();
    node_add(view,
             std::vector<std::string_view>{
                 "a", "invocations=8", "param=x=1", "param=y=2", "log=info"},
             {.where = "case:1", .beside = others});
    for (const std::vector<std::string_view> &words :
         std::vector<std::vector<std::string_view>>{{"invocations=4"},
                                                    {"param=x=3"},
                                                    {"log=warn", "log=info"},
                                                    {"vertex_count=3"},
                                                    {"param=z=1", "invocations=1"}})
      rows.row(
          std::format("[{}] -> {}", joined(words, " "), named_words(view, "a", words)));
  });
  // A node remove never takes the last of the nodes reading the terminal, by its view and
  // its node, or a node inside the one removed; while another reads it, any may go.
  bench.check("Edits.cpp/keep_terminal", [](Rows &rows) {
    const std::vector<std::vector<std::string_view>> readers{
        {}, {":term"}, {":ui.term"}, {":term", "tri:shell"}, {"tri:term"}};
    const std::vector<std::pair<std::string_view, std::string_view>> removed{
        {"", "term"}, {"", "ui"}, {"", "other"}, {"tri", "term"}, {"tri", "shell"}};
    const std::vector<std::string> said = rows.quiet([&] {
      std::vector<std::string> lines;
      Log log{Level::error, "bench"};
      SandboxFiles files;
      NoProcesses processes;
      for (const std::vector<std::string_view> &reading : readers) {
        Commands port{log, files, processes};
        for (const std::string_view reader : reading)
          port.reads_terminal(reader.substr(0, reader.find(':')),
                              reader.substr(reader.find(':') + 1));
        for (const auto &[view, node] : removed) {
          std::string answer = "passes";
          try {
            keep_terminal(port, view, node);
          } catch (...) {
            answer = thrown();
          }
          lines.push_back(std::format("readers [{}], {}:{} removed -> {}",
                                      joined(reading, " "),
                                      view,
                                      node,
                                      answer));
        }
      }
      return lines;
    });
    for (const std::string &line : said)
      rows.row(line);
  });
  // A connect word with no dot names no port: refused, suggesting a port of that node the
  // running view lists, one it writes for the first word, else one it reads, the first
  // the list gives of those, never a value; and a hosted view's window by its name.
  bench.check("Edits.cpp/check_ends", [](Rows &rows) {
    const std::vector<NodePort> ports{
        {.node = "a", .name = "in", .kind = "buffer", .writes = false},
        {.node = "a", .name = "out", .kind = "buffer", .writes = true},
        {.node = "a", .name = "k", .kind = "value", .writes = true},
        {.node = "b", .name = "out", .kind = "image", .writes = true},
        {.node = "b", .name = "in", .kind = "buffer", .writes = false},
        {.node = "c", .name = "k", .kind = "value", .writes = false},
        {.node = "d", .name = "out1", .kind = "buffer", .writes = true},
        {.node = "d", .name = "out2", .kind = "buffer", .writes = true},
        {.node = "d", .name = "in1", .kind = "buffer", .writes = false},
        {.node = "d", .name = "in2", .kind = "buffer", .writes = false}};
    const std::vector<std::string> hosted{"game", "a"};
    for (const std::vector<std::string_view> &line :
         std::vector<std::vector<std::string_view>>{{"n", "a", "b.in"},
                                                    {"n", "b.out", "a"},
                                                    {"n", "b", "a.in"},
                                                    {"n", "a.out", "b"},
                                                    {"n", "c", "a.in"},
                                                    {"n", "d", "a.in"},
                                                    {"n", "a.out", "d"},
                                                    {"n", "game", "a.in"},
                                                    {"n", "game:", "a.in"},
                                                    {"n", "a.out", "b.in", "zz"},
                                                    {"n", "a.out", "b.in"}}) {
      std::string said = "passes";
      try {
        check_ends(line, ports, hosted);
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("[{}] -> {}", joined(line, " "), said));
    }
  });
}

} // namespace VP::bench
