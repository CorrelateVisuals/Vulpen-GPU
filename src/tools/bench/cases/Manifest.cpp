#include "runtime/Manifest.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Worlds.h"

#include <string>
#include <string_view>
#include <vector>

// runtime/Manifest.cpp: a view's .vlp text both ways. A row's manifest is a file in the
// row's sandbox, at <root>/m/view.vlp; the claims over the tree read every view.vlp of
// projects/ and the recipes in place, and write nothing there (RA04).

namespace VP::bench {

namespace domain {
// A manifest's whole text: what a person writes, as they lay it out and as they get it
// wrong.
struct ManifestText {};
// A view's manifest from the sandbox, as its callers name one: in a folder of the view's
// name, a recipe's in the library's folders.
struct ViewFile {};
} // namespace domain

template <> struct Gen<domain::ViewFile> {
  using type = std::filesystem::path;
  static std::vector<std::filesystem::path> edges() {
    std::vector<std::filesystem::path> all;
    for (const std::u8string_view file : {u8"a/view.vlp",
                                          u8"a b/view.vlp",
                                          u8"\u00e9/view.vlp",
                                          u8"a/b/view.vlp",
                                          u8"a/x.vlp",
                                          u8"a/",
                                          u8"recipes/vulpen/library/parts/p/view.vlp",
                                          u8"recipes/vulpen/cli/view.vlp",
                                          u8"recipes/custom/mine/view.vlp",
                                          u8"a/./view.vlp",
                                          u8"a/b/../view.vlp"})
      all.emplace_back(file);
    return all;
  }
  static std::filesystem::path random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::ManifestText> {
  using type = std::string;
  static std::vector<std::string> edges() {
    const std::string head = "[manifest]\nversion = 1\n";
    return {
        head + "\n[node \"a\"]\ninvocations = 4\n",
        "",
        head,
        "[manifest]\n",
        "[manifest]\nversion = 2\n",
        "[manifest]\nversion = 0\n",
        "[manifest]\nversion = x\n",
        "[manifest]\nversion = 1\nversion = 1\n",
        "[manifest]\r\nversion = 1\r\n\r\n[node \"a\"]\r\ninvocations = 4\r\n",
        "# a comment\n" + head +
            "# about a\n[node \"a\"] # at the end\nlog = info # too\n",
        head + "[node \"a\"]\n[node \"a\"]\n",
        head + "[node a]\n",
        head + "[node \"\"]\n",
        head + "[node \"]\n", // a name that is one quote, both its ends
        head + "[node \"a\"\n",
        head + "[nodes \"a\"]\n",
        head + "[node \"a.b\"]\n",
        head + "[node \"a\"]\n[node \"a.b\"]\nparam = x=1\n",
        head + "[node \"a\"]\nparam = x=1\nparam = x=2\n",
        head + "[node \"a\"]\nparam = x = 1\n",
        head + "[node \"a\"]\nwhat = 1\n",
        head + "[node \"a\"]\ninvocations\n",
        head + "[node \"a\"]\nfile = A.comp\n",
        head + "word = 1\n",
        "version = 1\n[manifest]\n",
        head +
            "[node \"a\"]\n[node \"b\"]\n[connection \"c\"]\nfrom = a.out\nto = b.in\n",
        head +
            "[connection \"c\"]\nfrom = a.out\nto = b.in\n[node \"a\"]\n[node \"b\"]\n",
        head + "[node \"a\"]\n[connection \"c\"]\nfrom = a.out\n",
        head + "[node \"a\"]\n[node \"b\"]\n[connection \"c\"]\nfrom = a.out\nto = "
               "b.in\nto = "
               "b.in\n",
        head + "[view \"game\"]\n",
        head + "[view \"game\"]\nfile = ../elsewhere/view.vlp\n",
        head + "[view \"game\"]\n[node \"game\"]\n",
        head + "[node \"root\"]\nparam = x=1\n",
        head + "[node \"contracts\"]\n",
        head + "\n\n\n[node \"a\"]\n\n\nlog = info\n\n",
        head + "[node \"a\"]\nlog = info",
        head + "\xef\xbb\xbf[node \"a\"]\n",
        "\xef\xbb\xbf" + head,
    };
  }
  // Lines a manifest may hold, in any order.
  static std::string random(Random &random) {
    constexpr std::array<std::string_view, 12> lines{"[manifest]",
                                                     "version = 1",
                                                     "[node \"a\"]",
                                                     "[node \"b\"]",
                                                     "[node \"a.b\"]",
                                                     "param = x=1",
                                                     "log = info",
                                                     "invocations = 4",
                                                     "[connection \"c\"]",
                                                     "from = a.out",
                                                     "to = b.in",
                                                     "# note"};
    std::string text = "[manifest]\nversion = 1\n";
    for (std::uint64_t line = random.below(8); line > 0; --line)
      text.append(lines[random.below(lines.size())]).append("\n");
    return text;
  }
};

namespace {

std::filesystem::path manifest_in(const Rows &rows) {
  return rows.root() / "m" / manifest_file;
}

// What a save writes for a view loaded from a file, which a round trip compares with
// the file: the text function save writes, with the comments the file holds.
std::string saved_text(const std::filesystem::path &file) {
  Reader reader(file);
  const View view = reader.read();
  return text(view, reader.notes());
}

// The first line two texts differ at, as a row names it.
std::string first_difference(std::string_view one, std::string_view other) {
  const std::vector<std::string_view> ones = lines_in(one);
  const std::vector<std::string_view> others = lines_in(other);
  for (std::size_t line = 0; line < std::max(ones.size(), others.size()); ++line) {
    const std::string_view mine = line < ones.size() ? ones[line] : "(none)";
    const std::string_view theirs = line < others.size() ? others[line] : "(none)";
    if (mine != theirs)
      return std::format(
          "line {}: {} saved as {}", line + 1, escaped(mine), escaped(theirs));
  }
  return one == other ? "the same" : "the same lines, other line ends";
}

} // namespace

void cases(Bench &bench) {
  bench.sweep(
      "Manifest.cpp/Manifest::load",
      [](const Rows &rows, std::string_view text) {
        Files::save(manifest_in(rows), text);
        return worlds::whole(Manifest::load(manifest_in(rows)));
      },
      domain::ManifestText{});

  // What a save writes over a file that held this text: a view of node a whose log is
  // info, saved over each kind of manifest a person leaves, a broken one too, and what it
  // said of that file for the log.
  bench.sweep(
      "Manifest.cpp/Manifest::save",
      [](const Rows &rows, std::string_view before) {
        const std::filesystem::path file = manifest_in(rows);
        Files::save(file, before);
        View view = Manifest::empty(file);
        view.nodes.push_back({.name = "a", .log = "info"});
        const std::vector<std::string> said = Manifest::save(view);
        const std::string text = Files::text(file);
        return said.empty() ? text : std::format("{}; said {}", text, joined(said, "; "));
      },
      domain::ManifestText{});

  // A load and a save give back every manifest of the tree byte for byte (RA04).
  bench.check("Manifest.cpp/Manifest::save round trip", [](Rows &rows) {
    std::string failing;
    for (const std::filesystem::path &relative : worlds::manifests(rows.repo())) {
      const std::filesystem::path file = rows.repo() / relative;
      try {
        const std::string difference =
            first_difference(Files::text(file), saved_text(file));
        if (difference != "the same")
          failing += std::format("; {}: {}", relative.generic_string(), difference);
      } catch (const std::exception &refused) {
        failing +=
            std::format("; {}: {}", relative.generic_string(), escaped(refused.what()));
      }
    }
    rows.row(failing.empty()
                 ? "every view.vlp of projects/ and the recipes saves as it was"
                 : "these save otherwise" + failing);
  });

  // The words a section gives more than once, each line with comments of its own, above
  // it and after it: a save puts each back on its own line.
  bench.check(
      "Manifest.cpp/Manifest::save round trip, words given twice", [](Rows &rows) {
        const std::string head = "[manifest]\nversion = 1\n\n";
        const std::vector<std::pair<std::string, std::string>> texts{
            {"param",
             head + "[node \"a\"]\n# above x\nparam = x=1  # after x\n"
                    "# above y\nparam = y=2  # after y\n"},
            {"image",
             head + "[node \"a\"]\n# above atlas\nimage = atlas=R8_UNORM  # after atlas\n"
                    "# above mask\nimage = mask=R8G8B8A8_UNORM  # after mask\n"},
            {"to",
             head + "[node \"a\"]\n\n[node \"b\"]\n\n[connection \"c\"]\nfrom = a.out\n"
                    "# above in\nto   = b.in  # after in\n"
                    "# above other\nto   = b.other  # after other\n"},
        };
        for (const auto &[word, text] : texts) {
          Files::save(manifest_in(rows), text);
          rows.row(std::format("two {} lines -> {}",
                               word,
                               first_difference(text, saved_text(manifest_in(rows)))));
        }
      });

  // Every view of the tree unfolds as the schedule runs it, with the library's recipes.
  // What each view flattens to, with recipes of its own: its words as a save writes
  // them, as a hash, and the recipes kept with how many files they were read from, so a
  // node a recipe adds or names otherwise, or a read a kept recipe forgets, moves a row;
  // and the folders no node names, which hosting warns of.
  bench.check("Manifest.cpp/Manifest::flatten", [](Rows &rows) {
    for (const std::filesystem::path &relative : worlds::manifests(rows.repo())) {
      Recipes recipes;
      try {
        const View loaded = Manifest::load(rows.repo() / relative);
        const std::size_t unnamed = Manifest::unnamed(loaded).size();
        const View flat = Manifest::flatten(loaded, recipes);
        std::vector<std::string> kept;
        std::size_t reads = 0;
        for (const Recipes::Kept &each : recipes.kept) {
          kept.push_back(each.name);
          reads += each.read.size();
        }
        std::ranges::sort(kept);
        rows.row(
            std::format("{}: {} nodes, {} connections, words #{:016x}; kept [{}], read "
                        "from {} files; {} folders no node's",
                        relative.generic_string(),
                        flat.nodes.size(),
                        flat.connections.size(),
                        hash_of(text(flat, {})),
                        joined(kept, " "),
                        reads,
                        unnamed));
      } catch (const std::exception &refused) {
        rows.row(std::format(
            "{}: refused, {}", relative.generic_string(), escaped(refused.what())));
      }
    }
  });

  // Its callers pass an absolute path, as a manifest's folder and the command port
  // resolve them (RP02), so its rows' paths are from the sandbox.
  bench.sweep(
      "Manifest.cpp/Manifest::empty",
      [](const Rows &rows, const std::filesystem::path &file) {
        return worlds::whole(Manifest::empty(rows.root() / file));
      },
      domain::ViewFile{});

  bench.sweep("Manifest.cpp/listed", listed, domain::Name{});
  bench.sweep("Manifest.cpp/runs", runs);
  bench.sweep(
      "Manifest.cpp/place",
      [](std::string_view section, std::string_view key, std::string_view value) {
        return place(section, key, value);
      },
      domain::Name{},
      domain::Name{},
      domain::Word{});
}

} // namespace VP::bench
