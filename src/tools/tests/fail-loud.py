#!/usr/bin/env python3
"""Every broken view ends the run with exit 1 and an error that names its cause (A02,
RV03), before anything wrong reaches the GPU: a manifest that says a thing twice, SPIR-V
cut short, C++ and a shader that disagree on a name or a type, a connection whose ends
disagree, a dispatch that leaves a workgroup part full or counts no invocations, a draw
that writes, a node inside no node, a file named by a command, a node's command without
its help, registered twice or failing when it runs, a view that uses a library recipe
as it is, a library recipe that uses one the library lacks, uses itself, or names a
node the one it uses lacks, a C++ output of a type no other node can name, a C++ input
of another type than its writer's, a C++ struct whose members sit elsewhere than the
shader's, C++ writing more elements than its buffer holds, an image C++ fills that
nothing samples beside a Texture nothing fills, C++ uploading other than an image's
count of pixels, C++ pixels other than the image's format takes, a format there is not,
image clear of a port that fills no image, a key there is not, a buffer C++ fills for
another node's shader whose members sit elsewhere, given no room, or that nothing reads,
and a draw's image taken to no Texture, or given a format there is not.

A view with more pass blocks than one pool holds is no mistake: it runs, and so does an
edit on it. Nor is a command line on the terminal: what it reads runs, a refusal names
its cause and the next line still runs, and the run ends with its input. A pass reads the
frame block as the engine wrote it, each value where reflection put it, the cursor where
the pointer went. The input command's lines reach a node in the frame after, in order. A C++ connection
hands its reader what its writer wrote that frame, a node makes a buffer through the
engine by hand, and structs C++ writes reach the shader member for member. The pixels
C++ fills an image with reach a shader pixel for pixel, through the node's own Texture,
through a connection and as 16-bit floats, stay through a rebuild, and come back after
image clear when the node fills them again. The image a draw renders into reaches a
dispatch after it in the same frame, pixel for pixel, at the size --size gives a run
with no window. The library's palette, font, rects and glyphs draw the fixture's sign in
a window without an error, where a display is. A
view's child views run, and leave and come back by edits that save the manifest
unchanged. And a drop copies a recipe whose sync brings it up to the library's while it
is unchanged, keeping the params the view set, and refuses once the view changed the
copy; a sync of a copy of recipes that use others takes out the nodes, files and folders
the library let go, and names the params that went with them.

Each view is written into a folder named mistakes, beside a link to each node's folder
it names, so it finds the nodes the build compiles from mistakes/ beside this script,
which get these things wrong on purpose. The recipe cases write a library of their own.

Usage: python3 src/tools/tests/fail-loud.py VULPEN
"""
import re
import shutil
import sys
import tempfile
from pathlib import Path

from harness import display, problems, run

HEAD = "[manifest]\nversion = 1\n"
FILL = '[node "fill"]\ninvocations = 64\nparam = amount=1\n'
PAIRS = '[node "pairs"]\ninvocations = 64\n'
SUM = '[node "sum"]\ninvocations = 64\n'
DRAW = '[node "draw"]\nvertex_count = 3\n'
GIVE = '[node "give"]\noperator = Give\n'
TAKE = '[node "take"]\noperator = Take\n'
SHAPE = '[node "shape"]\noperator = Shapes\ninvocations = 4\n'
MAKER = '[node "maker"]\noperator = Maker\n'
PICTURE = '[node "picture"]\noperator = Picture\ninvocations = 8\nimage = picture=R8_UNORM\n'
SIGN = '[node "sign"]\noperator = Sign\n'
PAINT = '[node "paint"]\nvertex_count = 3\n'
LOOK = '[node "look"]\noperator = Look\ninvocations = 64\n'
PASSES = 1025  # one past what a pool of pass blocks in Pipelines.cpp holds
FIXTURE = Path(__file__).resolve().parent / "mistakes"
PARTS = Path(__file__).resolve().parents[2] / "recipes" / "parts"
# The library's parts that put text on screen, rects before glyphs, so text draws over
# the panels.
TEXT = ("palette", "font", "rects", "glyphs")
NODE = re.compile(r'^\[node "([^".]+)', re.MULTILINE)
CUT = ("truncated", "empty")  # nodes whose SPIR-V this script writes, cut short


def connection(name: str, source: str, *targets: str) -> str:
    return f'[connection "{name}"]\nfrom = {source}\n' + "".join(f"to = {t}\n" for t in targets)


def operator(name: str) -> str:
    return FILL.replace("invocations", f"operator = {name}\ninvocations")


# A shape whose shapes maker's C++ fills, through a connection.
MADE = SHAPE.replace("Shapes", "Sums") + connection("shapes", "maker.shapes", "shape.shapes")
# The image paint draws, which look samples.
PAINTED = connection("painted", "paint.color", "look.painted")

# What each view gets wrong, and what its error must say.
CASES = {
    "param-twice": (HEAD + FILL + "param = amount=2\n", "param amount is set twice"),
    "manifest-twice": (HEAD + "[manifest]\n" + FILL, "[manifest] is given twice"),
    "version-twice": (HEAD + "version = 1\n" + FILL, "version is set twice"),
    "connection-twice": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.values")
                         + connection("a", "pairs.pairs", "sum.values"),
                         "two connections are named a"),
    "port-twice": (HEAD + PAIRS + SUM
                   + connection("a", "pairs.pairs", "sum.values", "sum.values"),
                   "sum.values joins two connections"),
    "port-in-two": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.values")
                    + connection("b", "pairs.pairs", "sum.values"),
                    "pairs.pairs joins two connections"),
    "truncated-spirv": (HEAD + FILL.replace("fill", "truncated"),
                        "Truncated.comp.spv is truncated"),
    "empty-spirv": (HEAD + FILL.replace("fill", "empty"), "Empty.comp.spv is not SPIR-V"),
    "cpp-name": (HEAD + operator("WrongName"), "the operator sets level, which the shader"),
    "cpp-type": (HEAD + operator("WrongType"), "sets amount as a uint, but the shader"),
    "elements": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.values"),
                 "pairs writes 8-byte vec2, values reads 4-byte float"),
    "not-read": (HEAD + PAIRS + SUM + connection("a", "pairs.pairs", "sum.sums"),
                 "sums is not a buffer its shader reads"),
    "workgroups": (HEAD + FILL.replace("= 64", "= 100"),
                   "invocations = 100 is not a multiple of local_size_x = 64"),
    "dispatch-counts": (HEAD + FILL.replace("invocations", "vertex_count"),
                        "it runs a .comp, so it counts its invocations"),
    "draw-writes": (HEAD + DRAW, "a draw's shaders only read buffers"),
    "draw-counts": (HEAD + DRAW.replace("vertex_count", "invocations"),
                    "it draws, so it counts its vertex_count"),
    "inside-nothing": (HEAD + FILL + '[node "fill.inner.deeper"]\n',
                       "node fill.inner.deeper is inside fill.inner, which is no node"),
    "file-word": (HEAD + FILL, "a node's files are what its folder holds", "node set fill file=x\n"),
    "command-help": (HEAD + operator("NoHelp"), "command `fill help` needs a usage and a help"),
    "command-twice": (HEAD + operator("Twin") + '[node "twin"]\noperator = Twin\n',
                      "node twin: command `fill twin` registers twice"),
    "command-fails": (HEAD + operator("Refuse"), "command-fails.txt:1: refused, as the fixture",
                      "refuse\n"),
    "uses-in-view": (HEAD + '[node "x"]\nrecipe = fill\n',
                     "node x uses recipe fill as it is, as only the library's own recipes do"),
    "cpp-unnamed": (HEAD + GIVE.replace("Give", "Secret"),
                    "output count is a (anonymous namespace)::Hidden, in an unnamed namespace"),
    "cpp-input-type": (HEAD + GIVE + TAKE.replace("Take", "Mistyped")
                       + connection("count", "give.count", "take.count"),
                       ("input count is a std::vector<int",
                        "but give writes a vp_mistakes::Count of 8 bytes")),
    "cpp-members": (HEAD + SHAPE.replace("Shapes", "Misplaced"),
                    "shapes: member size is a float at byte 12 in C++, but a float at byte 8"),
    "overflow": (HEAD + SHAPE.replace("Shapes", "Overflow"),
                 "the operator writes 8 elements of shapes, which holds 4"),
    "images": (HEAD + PICTURE.replace("Picture", "Stray"),
               ("the operator fills image stray, which nothing samples",
                "picture samples nothing")),
    "pixels": (HEAD + PICTURE.replace("Picture", "Misfilled"),
               "the operator uploads 7 pixels to picture, which is 4 by 2"),
    "image-type": (HEAD + PICTURE.replace("R8_UNORM", "R32_SFLOAT"),
                   "the operator fills picture with uint8 pixels, but its format R32_SFLOAT "
                   "takes float"),
    "image-format": (HEAD + PICTURE.replace("R8_UNORM", "R8_UNROM"),
                     "image picture=R8_UNROM: no such format"),
    "clear-nothing": (HEAD + PICTURE, "picture.nothing is no image a node's C++ fills",
                      "image clear picture.nothing\n"),
    "input-key": (HEAD + FILL, "enterr is no key", "input key down enterr\n"),
    "made-members": (HEAD + MAKER.replace("Maker", "Mismade") + MADE,
                     "connection shapes: shapes: member size is a float at byte 12 in C++, "
                     "but a float at byte 8"),
    "made-room": (HEAD + MAKER.replace("Maker", "Roomless") + MADE,
                  "the operator writes shapes, which no shader of its node holds; give it room"),
    "made-unread": (HEAD + MAKER, "the operator writes shapes, which nothing reads"),
    "draw-reader": (HEAD + PAINT + SUM + connection("painted", "paint.color", "sum.values"),
                    "connection painted: paint draws an image, which values is no Texture"),
    "draw-format": (HEAD + PAINT + "image = color=R8G8B8A8_UNROM\n" + LOOK + PAINTED,
                    "image color=R8G8B8A8_UNROM: no such format"),
}
WINDOWED = {"draw-writes", "draw-counts"}

# A library of manifests only, for the recipe cases: each runs as the view at its path,
# from the library's folder.
LIBRARY = {
    "parts/loop": '[node "loop"]\n\n[node "loop.again"]\nrecipe = loop\n',
    "parts/part": '[node "part"]\n',
    "apps/missing": '[node "missing"]\n\n[node "missing.x"]\nrecipe = nothing\n',
    "apps/param": '[node "param"]\n\n[node "param.x"]\nrecipe = part\nparam = nothing.amount=1\n',
    "apps/port": ('[node "port"]\n\n[node "port.x"]\nrecipe = part\n\n'
                  + connection("values", "port.x.nothing.values", "port.x.values")),
}
RECIPE_CASES = {
    "apps/missing": ("missing/view.vlp:6: node missing.x: ",
                     "/recipes/parts/nothing/view.vlp: cannot be read"),
    "parts/loop": "the recipes it uses form a cycle through loop (RV06)",
    "apps/param": "it sets param nothing.amount, but recipe part has no node part.nothing",
    "apps/port": "connection values joins port.x.nothing, which is no node",
}


def mirror(vulpen: str) -> Path:
    """Where the build put the fixture's modules and SPIR-V, whole, so a link there finds
    what it names."""
    return Path(vulpen).resolve().parent / "views" / "mistakes"


def cut_spirv(vulpen: str) -> list[Path]:
    """SPIR-V no build writes, for nodes whose folders hold only a shader's name: one cut
    inside its first instruction, one empty."""
    whole = (mirror(vulpen) / "fill" / "Fill.comp.spv").read_bytes()
    header_and_a_word = 24
    written = []
    for node, content in zip(CUT, (whole[:header_and_a_word], b"")):
        folder = mirror(vulpen) / node
        folder.mkdir(exist_ok=True)
        (folder / f"{node.capitalize()}.comp.spv").write_bytes(content)
        written.append(folder)
    return written


def view_in(folder: Path, name: str, text: str) -> Path:
    """The view, written into a folder named mistakes with a link to each node's folder
    it names: the fixture's, a fill's for a node named fillN, or one holding only a
    shader's name for a node whose SPIR-V is cut."""
    home = folder / name / "mistakes"
    home.mkdir(parents=True)
    for node in set(NODE.findall(text)):
        if node in CUT:
            (home / node).mkdir()
            (home / node / f"{node.capitalize()}.comp").write_text("", encoding="utf-8")
        elif (FIXTURE / node).is_dir():
            (home / node).symlink_to(FIXTURE / node, target_is_directory=True)
        elif node.startswith("fill"):
            (home / node).symlink_to(FIXTURE / "fill", target_is_directory=True)
    view = home / f"{name}.vlp"
    view.write_text(text, encoding="utf-8")
    return view


def check(vulpen: str, folder: Path, name: str, text: str, cause: str | tuple[str, ...],
          script: str = "") -> str | None:
    """Why the case failed, or None. A cause in parts is a path between them; a script
    runs through --source."""
    view = view_in(folder, name, text)
    arguments = [view, "--frames", 1, "--fps", 0]
    if script:
        (view.parent / f"{name}.txt").write_text(script, encoding="utf-8")
        arguments += ["--source", view.parent / f"{name}.txt"]
    code, output = run(vulpen, arguments, headless=name not in WINDOWED)
    return judge(name, code, output, cause)


def judge(name: str, code: int, output: str, cause: str | tuple[str, ...]) -> str | None:
    if code != 1 or any(part not in output for part in
                        ((cause,) if isinstance(cause, str) else cause)) or problems(output):
        return f"{name}: expected exit 1 and '{cause}', got exit {code}:\n{output}"
    return None


def recipes(vulpen: str, folder: Path) -> list[str]:
    """Why each recipe case failed: each runs the library recipe at its path as a view."""
    library = folder / "library" / "recipes"
    for path, text in LIBRARY.items():
        (library / path).mkdir(parents=True)
        (library / path / "view.vlp").write_text(HEAD + "\n" + text, encoding="utf-8")
    failed = []
    for path, cause in RECIPE_CASES.items():
        code, output = run(vulpen, [library / path / "view.vlp", "--frames", 1, "--fps", 0])
        if problem := judge(path, code, output, cause):
            failed.append(problem)
    return failed


def no_limit(vulpen: str, folder: Path) -> str | None:
    """Why a view past one pool of pass blocks, or an edit on it, failed, or None. Each
    node's folder is a link to fill's, and so is its folder in the build tree."""
    text = HEAD + "".join(FILL.replace('"fill"', f'"fill{index}"') for index in range(PASSES))
    view = view_in(folder, "passes", text)
    built = [mirror(vulpen) / f"fill{index}" for index in range(PASSES)]
    try:
        for link in built:
            link.symlink_to(mirror(vulpen) / "fill", target_is_directory=True)
        edit = view.parent / "edit.txt"
        edit.write_text("param set fill0 amount 2\n", encoding="utf-8")
        code, output = run(vulpen, [view, "--frames", 1, "--fps", 0, "--source", edit])
    finally:
        for link in built:
            link.unlink(missing_ok=True)
    if code != 0 or problems(output):
        return (f"passes: expected {PASSES} passes and an edit on them to run, got exit "
                f"{code}:\n{output}")
    return None


def terminal(vulpen: str, folder: Path) -> str | None:
    """Why lines piped to a node that reads the terminal did not run as commands, or None."""
    view = view_in(folder, "terminal", HEAD + operator("Echo"))
    code, output = run(vulpen, [view], typed="echo one two\nnode remove nothing\necho three")
    said = [line.strip() for line in output.splitlines()]
    if (code != 0 or problems(output) or "one two" not in said or "three" not in said
            or not any(line.endswith("no node is named nothing") for line in said)
            or said.index("one two") > said.index("three")):
        return f"terminal: expected the lines to run and the run to end, got exit {code}:\n{output}"
    return None


def frame_block(vulpen: str, folder: Path) -> str | None:
    """Why a pass did not read the frame block as the engine wrote it, or None; the
    fixture's operator stops when it differs, from a frame far in, where time is large,
    and with the pointer moved, which the cursor follows."""
    view = view_in(folder, "frame", HEAD + '[node "frame"]\noperator = Frame\ninvocations = 6\n')
    (view.parent / "pointer.txt").write_text("input pointer 12.5 40\n", encoding="utf-8")
    code, output = run(vulpen, [view, "--frames", 4, "--fps", 0, "--first-frame", 1_000_000,
                                "--source", view.parent / "pointer.txt"])
    if code != 0 or problems(output):
        return f"frame: expected the frame block as the engine wrote it, got exit {code}:\n{output}"
    return None


def cpp(vulpen: str, folder: Path) -> str | None:
    """Why C++ did not reach what it should, or None: a C++ connection hands its reader
    what its writer wrote that frame, a node makes a buffer through the engine by hand,
    and the structs C++ writes reach the shader member for member, its own or, through a
    connection, another node's; each node stops when it does not."""
    view = view_in(folder, "cpp", HEAD + GIVE + TAKE + '[node "scratch"]\noperator = Scratch\n'
                   + SHAPE + connection("count", "give.count", "take.count"))
    code, output = run(vulpen, [view, "--frames", 3, "--fps", 0])
    if code != 0 or problems(output):
        return f"cpp: expected each node to find what it checks, got exit {code}:\n{output}"
    made = view_in(folder, "made", HEAD + MAKER + MADE)
    code, output = run(vulpen, [made, "--frames", 3, "--fps", 0])
    if code != 0 or problems(output):
        return f"made: expected shapes C++ filled to reach the shader, got exit {code}:\n{output}"
    return None


def images(vulpen: str, folder: Path) -> str | None:
    """Why the pixels C++ filled an image with did not reach the shader, or None: through
    the node's own Texture, through a connection to it, as 16-bit floats no byte holds,
    and filled again after image clear. The fixture's operator adds a node after it
    fills, which rebuilds the view, and stops when a pixel differs."""
    relayed = PICTURE.replace("Picture", "Relayed").replace("picture=", "copy=")
    for name, text in (("own", HEAD + PICTURE),
                       ("relayed", HEAD + relayed
                        + connection("copy", "picture.copy", "picture.picture")),
                       ("deep", HEAD + PICTURE.replace("Picture", "Deep")
                        .replace("R8_UNORM", "R16G16B16A16_SFLOAT")),
                       ("cleared", HEAD + PICTURE.replace("Picture", "Cleared"))):
        view = view_in(folder, f"image-{name}", text)
        code, output = run(vulpen, [view, "--frames", 5, "--fps", 0])
        if code != 0 or problems(output):
            return f"image-{name}: expected the pixels C++ filled, got exit {code}:\n{output}"
    return None


def windowed(vulpen: str, folder: Path, name: str, parts: tuple[str, ...], nodes: str,
             connections: str) -> tuple[int, str]:
    """The exit code and output of a view of fixture nodes and library parts run in a
    window. Each part's node is its library manifest's, after the fixture's nodes, and its
    folder and build output are links to the library's, as the build compiles only the
    views it knows."""
    sections = "".join("\n" + section[section.index("[node "):] for section in
                       ((PARTS / part / "view.vlp").read_text(encoding="utf-8")
                        for part in parts))
    view = view_in(folder, name, HEAD + nodes + sections + connections)
    built = mirror(vulpen).parent / "library" / "parts"
    links = [mirror(vulpen) / part for part in parts]
    try:
        for part, link in zip(parts, links):
            (view.parent / part).symlink_to(PARTS / part, target_is_directory=True)
            link.symlink_to(built / part, target_is_directory=True)
        return run(vulpen, [view, "--frames", 60, "--fps", 0], headless=False)
    finally:
        for link in links:
            link.unlink(missing_ok=True)


def text(vulpen: str, folder: Path) -> str | None:
    """Why the library's palette, font, rects and glyphs did not draw the fixture's sign
    in a window without an error, or None; with no display, None. What they draw is
    checked by eye until a test can read the window back."""
    if not display():
        return None
    code, output = windowed(
        vulpen, folder, "text", TEXT, SIGN,
        connection("palette", "palette.palette", "rects.palette", "glyphs.palette")
        + connection("font", "font.font", "glyphs.font")
        + connection("atlas", "font.atlas", "glyphs.atlas")
        + connection("rects", "sign.rects", "rects.rects")
        + connection("labels", "sign.labels", "glyphs.labels")
        + connection("characters", "sign.characters", "glyphs.characters"))
    if code != 0 or problems(output):
        return f"text: expected the sign drawn without an error, got exit {code}:\n{output}"
    return None


def offscreen(vulpen: str, folder: Path) -> str | None:
    """Why the image a draw renders into did not reach a dispatch after it in the same
    frame, pixel for pixel, at the size --size gives a run with no window (V07), or
    None."""
    view = view_in(folder, "offscreen", HEAD + PAINT + LOOK + PAINTED)
    code, output = run(vulpen, [view, "--frames", 4, "--fps", 0, "--size", "64x48"])
    if code != 0 or problems(output):
        return f"offscreen: expected each pixel paint drew, got exit {code}:\n{output}"
    return None


def input_lines(vulpen: str, folder: Path) -> str | None:
    """Why the input command's lines did not reach a node as the window's events would,
    or None: in their order, in the first frame, and only in it."""
    view = view_in(folder, "input", HEAD + '[node "input"]\noperator = Input\n')
    lines = ("input key down a", "input text hello  world", "input pointer 12.5 40",
             "input button down left", "input wheel 0 -1", "input focus off",
             "input key up enter")
    (view.parent / "lines.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    code, output = run(vulpen, [view, "--frames", 2, "--fps", 0,
                                "--source", view.parent / "lines.txt"])
    if code != 0 or problems(output):
        return f"input: expected each line's event in the first frame, got exit {code}:\n{output}"
    return None


def children(vulpen: str, folder: Path) -> str | None:
    """Why a view's child view did not run, or did not leave and come back by edits that
    save the manifest as it was, or None."""
    host = folder / "children" / "host"
    (host / "inner").mkdir(parents=True)
    text = HEAD + '\n[view "inner"]\n'
    (host / "view.vlp").write_text(text, encoding="utf-8")
    (host / "inner" / "view.vlp").write_text(HEAD, encoding="utf-8")
    script = host / "edits.txt"
    script.write_text("child remove inner\nchild add inner inner/view.vlp\nview save\n",
                      encoding="utf-8")
    code, output = run(vulpen, [host / "view.vlp", "--frames", 1, "--fps", 0, "--log", "info",
                                "--source", script])
    saved = (host / "view.vlp").read_text(encoding="utf-8")
    if code != 0 or problems(output) or "child inner: hosted from" not in output or saved != text:
        return f"children: expected the child to run and the same manifest, got exit {code}:\n{output}\n{saved}"
    return None


def drops(vulpen: str, folder: Path) -> str | None:
    """Why a drop, or a sync of it, did not do what the library says, or None. The CLI
    runs from a copy of the library, which also holds a recipe of one data file, so no
    build serves it; its node runs nothing and fails only on its param, which it never
    reads."""
    root = Path(__file__).resolve().parents[2] / "recipes"
    library = folder / "drops" / "recipes"
    for part in ("command-line", "inspect", "library"):
        shutil.copytree(root / "parts" / part, library / "parts" / part)
    shutil.copytree(root / "apps" / "cli", library / "apps" / "cli")
    notes = library / "parts" / "notes"
    notes.mkdir()
    (notes / "view.vlp").write_text(
        HEAD + '\n[node "notes"]\nfile  = notes.ini\nparam = level=1\n', encoding="utf-8")
    project = folder / "drops" / "project"
    project.mkdir()
    (project / "view.vlp").write_text(HEAD, encoding="utf-8")
    copy = project / "copy" / "notes.ini"

    def cli(*lines: str) -> str:
        typed = "\n".join([f"view load {project}", *lines, "view save"]) + "\n"
        _, output = run(vulpen, [library / "apps" / "cli" / "view.vlp"], typed=typed)
        return output

    steps = [
        ("one", ("recipe drop notes copy", "param set copy level 5"),
         lambda out: copy.read_text() == "one" and "recipe = notes@" in manifest()),
        ("two", ("recipe sync copy",),
         lambda out: copy.read_text() == "two" and re.search(r"param +=", manifest())
         and "level=5" in manifest()),
        ("three", ("recipe sync copy",),
         lambda out: copy.read_text() == "mine" and "node copy changed since it was dropped" in out),
    ]

    def manifest() -> str:
        return (project / "view.vlp").read_text(encoding="utf-8")

    for content, lines, holds in steps:
        (notes / "notes.ini").write_text(content, encoding="utf-8")
        output = cli(*lines)
        if problems(output) or not holds(output):
            return f"drops: the library's notes as {content!r} went wrong:\n{output}\n{manifest()}"
        if content == "two":
            copy.write_text("mine", encoding="utf-8")
    both = library / "components" / "both"
    both.mkdir(parents=True)
    used = '[node "both"]\n\n[node "both.left"]\nrecipe = notes\n'
    (both / "view.vlp").write_text(
        HEAD + "\n" + used + '\n[node "both.right"]\nrecipe = notes\n\n'
        + connection("link", "both.left.out", "both.right.in"), encoding="utf-8")
    dropped = cli("recipe drop both pair")
    (both / "view.vlp").write_text(HEAD + "\n" + used, encoding="utf-8")
    synced = cli("recipe sync pair")
    if (problems(dropped + synced) or "let go of param level=1 of pair.right" not in synced
            or "pair.link" in manifest() or "pair.right" in manifest()
            or (project / "pair" / "right").exists()
            or not (project / "pair" / "left" / "notes.ini").is_file()):
        return f"drops: a sync kept what the library let go:\n{dropped}\n{synced}\n{manifest()}"
    return None


def main() -> None:
    vulpen = sys.argv[1]
    cut = cut_spirv(vulpen)
    try:
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            failed = [problem for name, (text, cause, *script) in CASES.items()
                      if name not in WINDOWED or display()
                      if (problem := check(vulpen, folder, name, text, cause, *script))]
            failed += recipes(vulpen, folder)
            for problem in (no_limit(vulpen, folder), terminal(vulpen, folder),
                            frame_block(vulpen, folder), cpp(vulpen, folder),
                            images(vulpen, folder), text(vulpen, folder),
                            offscreen(vulpen, folder),
                            input_lines(vulpen, folder),
                            children(vulpen, folder),
                            drops(vulpen, folder)):
                if problem:
                    failed.append(problem)
    finally:
        for made in cut:
            shutil.rmtree(made)
    if failed:
        sys.exit("\n".join(failed))


if __name__ == "__main__":
    main()
