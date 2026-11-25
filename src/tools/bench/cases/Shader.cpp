#include "baseclasses/Shader.cpp"

#include "baseclasses/Platform.h"
#include "runtime/Text.h"
#include "tools/bench/Bench.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

// baseclasses/Shader.cpp: SPIR-V reflection (RA03). The build's own shaders are the
// worlds: every one but the fail-loud test's reflects, those of src/tools/tests/mistakes
// say what they say, a row each, and a damaged copy of any, cut short or with a word
// changed, as a disk or a killed build may leave one, is refused naming its file, or
// read; never anything else (A02). They are the debug build's, as every preset compiles
// a shader alike and only a full build compiles the views'.

namespace VP::bench {

template <> struct Shown<Field> {
  static std::string text(const Field &field, Names &names) {
    std::string text = std::format("{} {} at {}", field.type, field.name, field.offset);
    if (field.buffer())
      text += std::format(" stride {} {}",
                          field.stride,
                          field.access == Access::read    ? "read"
                          : field.access == Access::write ? "write"
                                                          : "read_write");
    if (!field.members.empty())
      text += " members " + show(field.members, names);
    return text;
  }
};

template <> struct Shown<Varying> {
  static std::string text(const Varying &varying, Names &) {
    return std::format("{} {} at {}", varying.type, varying.name, varying.location);
  }
};

namespace {

std::filesystem::path build_folder(const Rows &rows) {
  return rows.repo() / "out" / "build" / "debug";
}

// Every shader the build compiled, by its path from the build's folder, sorted; the
// fail-loud test's mistakes alone or all but them.
std::vector<std::filesystem::path> shaders(const Rows &rows, bool mistakes) {
  std::vector<std::filesystem::path> found;
  const std::filesystem::path build = build_folder(rows);
  if (!std::filesystem::exists(build))
    return found;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(build))
    if (entry.is_regular_file() && entry.path().extension() == ".spv") {
      const std::filesystem::path relative = entry.path().lexically_relative(build);
      if ((relative.generic_string().starts_with("views/mistakes/")) == mistakes &&
          !relative.generic_string().starts_with("bench/"))
        found.push_back(relative);
    }
  std::ranges::sort(found);
  return found;
}

std::string reflected(const Shader &shader) {
  Names names;
  return std::format("fields {} block {} frame {} frame_size {} workgroup {} outputs {} "
                     "inputs {}",
                     show(shader.fields(), names),
                     shader.block_size(),
                     show(shader.frame(), names),
                     shader.frame_size(),
                     show(shader.workgroup_size(), names),
                     show(shader.outputs(), names),
                     show(shader.inputs(), names));
}

// A copy of the words damaged as the seed picks: cut after some word, one bit flipped,
// a word zeroed or filled, or an instruction's word count made huge.
std::string damaged(const std::string &bytes, Random &random, std::string &how) {
  std::string copy = bytes;
  const std::size_t words = copy.size() / 4;
  const std::size_t word = 5 + random.below(words > 5 ? words - 5 : 1);
  switch (random.below(5)) {
    case 0:
      copy.resize(word * 4);
      how = std::format("cut after word {}", word);
      break;
    case 1: {
      const std::size_t bit = random.below(32);
      copy[word * 4 + bit / 8] =
          static_cast<char>(copy[word * 4 + bit / 8] ^ (1 << (bit % 8)));
      how = std::format("word {} bit {} flipped", word, bit);
      break;
    }
    case 2:
      copy.replace(word * 4, 4, 4, '\0');
      how = std::format("word {} zeroed", word);
      break;
    case 3:
      copy.replace(word * 4, 4, 4, '\xff');
      how = std::format("word {} filled", word);
      break;
    default:
      copy[word * 4 + 3] = '\x7f'; // the high half of the first word is its word count
      how = std::format("word {} counts 32,000 words", word);
  }
  return copy;
}

} // namespace

void cases(Bench &bench) {
  bench.check("Shader.cpp/Shader::Shader every shader of the build", [](Rows &rows) {
    std::string failing;
    const std::filesystem::path build = build_folder(rows);
    for (const std::filesystem::path &shader : shaders(rows, false)) {
      try {
        Shader reflect(build / shader);
      } catch (const std::exception &refused) {
        failing +=
            std::format("; {}: {}", shader.generic_string(), escaped(refused.what()));
      }
    }
    rows.row(failing.empty() ? "every shader of the build but the mistakes' reflects"
                             : "these are refused" + failing);
  });

  bench.check("Shader.cpp/Shader::Shader the mistakes' shaders", [](Rows &rows) {
    const std::filesystem::path build = build_folder(rows);
    for (const std::filesystem::path &shader : shaders(rows, true)) {
      std::string said;
      try {
        said = reflected(Shader(build / shader));
      } catch (const std::exception &refused) {
        said = "throws " + escaped(refused.what());
      }
      rows.row(std::format("{} -> {}", shader.generic_string(), said));
    }
  });

  // The first compute shader of the build with a word of its local size zeroed, each of
  // the three in turn, as a damaged file may hold it: a dispatch's workgroups are its
  // invocations divided by that size.
  bench.check("Shader.cpp/Shader::Shader a local size zeroed", [](Rows &rows) {
    const std::filesystem::path build = build_folder(rows);
    const std::vector<std::filesystem::path> all = shaders(rows, false);
    const auto compute =
        std::ranges::find_if(all, [](const std::filesystem::path &shader) {
          return shader.generic_string().ends_with(".comp.spv");
        });
    if (compute == all.end()) {
      rows.row("no compute shader: the debug build makes them");
      return;
    }
    const std::string bytes = Files::text(build / *compute);
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(std::uint32_t));
    // OpExecutionMode (16) whose mode is LocalSize (17): x, y and z follow.
    std::size_t at = spirv::header_words;
    while (at + 5 < words.size() &&
           !((words[at] & spirv::opcode_mask) == spirv::op_execution_mode &&
             words[at + 2] == spirv::mode_local_size))
      at += std::max<std::size_t>(words[at] >> spirv::word_count_shift, 1);
    if (at + 5 >= words.size()) {
      rows.row(compute->generic_string() + " holds no local size");
      return;
    }
    const std::filesystem::path file = rows.root() / "zeroed.spv";
    for (std::size_t operand = 0; operand < 3; ++operand) {
      std::vector<std::uint32_t> zeroed = words;
      zeroed[at + 3 + operand] = 0;
      Files::save(file,
                  std::string(reinterpret_cast<const char *>(zeroed.data()),
                              zeroed.size() * sizeof(std::uint32_t)));
      std::string said;
      try {
        Names names;
        said = "workgroup " + show(Shader(file).workgroup_size(), names);
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{}, its local size's {} zeroed -> {}",
                           compute->generic_string(),
                           "xyz"[operand],
                           said));
    }
  });

  // Thirty damaged copies of each shader, each seeded by the shader's path.
  bench.check("Shader.cpp/Shader::Shader a damaged shader", [](Rows &rows) {
    constexpr std::size_t copies = 30;
    constexpr std::size_t shown_failures = 12;
    const std::filesystem::path build = build_folder(rows);
    const std::filesystem::path file = rows.root() / "damaged.spv";
    std::size_t failed = 0;
    std::size_t tried = 0;
    for (const std::filesystem::path &shader : shaders(rows, false)) {
      const std::string bytes = Files::text(build / shader);
      Random random(hash_of(shader.generic_string()));
      for (std::size_t copy = 0; copy < copies; ++copy) {
        std::string how;
        Files::save(file, damaged(bytes, random, how));
        ++tried;
        // Named first, so a copy that ends the process leaves its name; dropped once it
        // did not.
        std::fprintf(
            stderr, "reading %s, %s\n", shader.generic_string().c_str(), how.c_str());
        try {
          Shader reflect(file);
          rows.skip();
        } catch (const std::runtime_error &refused) {
          rows.skip();
          if (std::string_view(refused.what()).find(file.string()) !=
              std::string_view::npos)
            continue;
          if (++failed <= shown_failures)
            rows.row(std::format("fails: {}, {}: refused naming no file: {}",
                                 shader.generic_string(),
                                 how,
                                 escaped(refused.what())));
        } catch (...) {
          rows.skip();
          if (++failed <= shown_failures)
            rows.row(
                std::format("fails: {}, {}: {}", shader.generic_string(), how, thrown()));
        }
      }
    }
    rows.row(failed == 0
                 ? "holds: a damaged shader is refused naming its file, or read"
                 : std::format("fails: a damaged shader is refused naming its file, or "
                               "read, in {} of the copies",
                               failed == tried ? "all" : "some"));
  });
}

} // namespace VP::bench
