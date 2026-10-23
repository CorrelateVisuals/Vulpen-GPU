"""What the test scripts share: running vulpen as a user would, and reading what it said.

A run ends in a problem when anything prints a Vulkan validation message or a sanitizer
report (RVK00, A03), whatever else it checks.
"""
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
EXAMPLES = ROOT / "projects"  # the example projects, wave and triangle
PROBLEM = re.compile(r"Validation (?:Error|Warning|Performance Warning):|runtime error:"
                     r"|ERROR: \w+Sanitizer|WARNING: ThreadSanitizer")
OUT = re.compile(r"\{out\} (.*)$")
DEVICE = re.compile(r"\{gpu\} runs on (.*)$")


def environment(headless: bool = True) -> dict[str, str]:
    """A headless run takes the driver the build chose for tests (lavapipe), when it
    found one; a window takes the machine's own GPU, and so does every run when
    VULPEN_OWN_GPU is set."""
    env = dict(os.environ)
    driver = env.get("VULPEN_TEST_DRIVER")
    if headless and driver and not env.get("VULPEN_OWN_GPU"):
        env["VK_DRIVER_FILES"] = driver
    return env


def run(vulpen: str, arguments: list, headless: bool = True,
        timeout: float = 120, env: dict[str, str] | None = None,
        typed: str = "") -> tuple[int, str]:
    """The exit code and everything the run printed; typed is its standard input."""
    done = subprocess.run([vulpen, *map(str, arguments)], capture_output=True, text=True,
                          errors="replace", timeout=timeout, input=typed,
                          env=env if env is not None else environment(headless))
    return done.returncode, done.stdout + done.stderr


def problems(output: str) -> list[str]:
    return [line for line in output.splitlines() if PROBLEM.search(line)]


def matches(pattern: re.Pattern, output: str) -> list[str]:
    return [found.group(1) for line in output.splitlines() if (found := pattern.search(line))]


def display() -> bool:
    """Whether a window can open: Windows names no display, as its desktop always has
    one; elsewhere X or Wayland names it."""
    return os.name == "nt" or bool(os.environ.get("DISPLAY")
                                   or os.environ.get("WAYLAND_DISPLAY"))


def link_folder(link: Path, folder: Path) -> None:
    """Makes link a link to folder. Windows makes a symbolic link only for an
    administrator or in Developer Mode, but a junction, which leads to a folder alike,
    for anyone."""
    if os.name == "nt":
        import _winapi  # CPython's own way to a junction, as os makes none
        _winapi.CreateJunction(str(folder), str(link))
    else:
        link.symlink_to(folder, target_is_directory=True)
