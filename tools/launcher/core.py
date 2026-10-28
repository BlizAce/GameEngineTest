"""Repository operations behind the Hollowpoint launcher.

Everything the launcher's buttons do lives here rather than in the Qt layer, for the reason AGENTS.md gives for any
untestable code path: a build step written inside a widget callback cannot be tested, and a launcher that silently runs the
wrong command is worse than a terminal. This module knows nothing about Qt — it composes commands, runs them, and reports.

The commands themselves are not invented here. They are exactly what README.md documents (``cmake --preset``,
``cmake --build build/debug -j``, ``ctest --test-dir``, the two executables), because a launcher that grows its own build
invocation is a second source of truth about how to build the engine — and the first one people read is the README.
"""

from __future__ import annotations

import ctypes.util
import dataclasses
import os
import platform
import queue
import shutil
import subprocess
import sys
import threading
from collections.abc import Iterator
from pathlib import Path

# How many lines of output a completed task keeps for the UI to scroll back through. Unbounded growth is a leak in a tool
# left open all day with a build streaming into it; 5000 lines covers a full failed compile of any module here.
MAX_LOG_LINES = 5000

# Third-party dependencies land here, matching CMakeFetchContent's default location under the repository so a launcher
# install and a plain `cmake --preset` see the same tree rather than two copies of Vulkan headers.
DEPENDENCY_CACHE = Path.home() / ".cache" / "hollowpoint-deps"

# Preset names from CMakePresets.json, and the binary dir each one maps to. Kept as data (not hardcoded paths) because a
# preset whose binaryDir changed would otherwise make the launcher operate on a stale build tree with total confidence.
PRESETS: dict[str, str] = {
    "linux-debug": "build/debug",
    "linux-release": "build/release",
    "linux-relwithdebinfo": "build/relwithdebinfo",
}


@dataclasses.dataclass(frozen=True)
class Requirement:
    """One toolchain prerequisite, with the package name to install it on this platform."""

    name: str
    binary: str | None
    purpose: str
    packages: dict[str, str]  # platform.system()-style key -> install hint

    def is_available(self) -> bool:
        if self.binary is None:
            return True
        return shutil.which(self.binary) is not None

    def install_hint(self) -> str:
        """A copy-pasteable install command for this OS, or a generic pointer when the key is unknown."""
        system = platform.system()
        for key, hint in self.packages.items():
            if key.lower() in system.lower():
                return hint
        return f"install {self.name} using your package manager"


REQUIREMENTS: tuple[Requirement, ...] = (
    Requirement(
        "CMake",
        "cmake",
        "configure and build (>= 3.25)",
        {"Linux": "sudo dnf install cmake", "Darwin": "brew install cmake", "Windows": "winget install Kitware.CMake"},
    ),
    Requirement(
        "Ninja",
        "ninja",
        "build backend",
        {"Linux": "sudo dnf install ninja-build", "Darwin": "brew install ninja", "Windows": "winget install Ninja.Ninja"},
    ),
    Requirement(
        "C++20 compiler",
        None,  # checked separately: either of two names counts, so a single binary is the wrong model
        "g++ or clang++ with C++20",
        {"Linux": "sudo dnf install gcc-c++", "Darwin": "xcode-select --install", "Windows": "winget install Kitware.VSBuildTools"},
    ),
    Requirement(
        "Git",
        "git",
        "fetch the pinned third-party sources",
        {"Linux": "sudo dnf install git", "Darwin": "brew install git", "Windows": "winget install Git.Git"},
    ),
)


def has_cpp_compiler() -> bool:
    """True when a C++20-capable compiler is on PATH.

    Two names count (g++, clang++, plus MSVC's cl) because the engine supports GCC and clang on Linux/macOS and MSVC on
    Windows; checking one name would report "install a compiler" on every macOS machine that has exactly the other.
    """
    return any(shutil.which(name) for name in ("g++", "clang++", "c++", "cl"))


def check_toolchain() -> list[tuple[Requirement, bool]]:
    """Each requirement with whether it is present. Order is preserved so the UI lists tools in a stable sequence."""
    results: list[tuple[Requirement, bool]] = []
    for requirement in REQUIREMENTS:
        available = requirement.is_available()
        if requirement.binary is None:
            available = has_cpp_compiler()
        results.append((requirement, available))
    return results


def discover_projects(projects_dir: Path) -> list[Path]:
    """`.hproject` files under `projects_dir`, sorted.

    Scanning the filesystem rather than a launcher-side config file means a project added by hand or by another tool shows
    up immediately — the alternative is a registry that goes stale and tells an author their project does not exist.
    """
    if not projects_dir.is_dir():
        return []
    return sorted(projects_dir.glob("*.hproject"))


def build_commands(preset: str = "linux-debug") -> dict[str, list[str]]:
    """The command lines the launcher runs, keyed by action.

    Single source for both the UI and its tests: an action's behaviour is asserted here on the argv it produces, which is
    the part that can be wrong in a way the user sees as "it rebuilt the wrong thing".
    """
    binary_dir = PRESETS.get(preset, PRESETS["linux-debug"])
    return {
        "install_deps": ["cmake", "--preset", preset],
        "rebuild": ["cmake", "--build", binary_dir, "-j"],
        "configure": ["cmake", "--preset", preset],
        "test": ["ctest", "--test-dir", binary_dir, "-j4"],
        "run_editor": [str(Path(binary_dir) / "output" / "hollowpoint-editor")],
        "run_runtime": [str(Path(binary_dir) / "output" / "hollowpoint-runtime")],
    }


def editor_binary(repo_root: Path, preset: str = "linux-debug") -> Path:
    """Where the editor executable lives after a build of `preset`."""
    return repo_root / PRESETS.get(preset, PRESETS["linux-debug"]) / "output" / "hollowpoint-editor"


def is_configured(repo_root: Path, preset: str = "linux-debug") -> bool:
    """True when a preset has been configured (a CMakeCache exists), so Rebuild will not fail on a missing tree."""
    return (repo_root / PRESETS.get(preset, PRESETS["linux-debug"]) / "CMakeCache.txt").is_file()


def is_built(repo_root: Path, preset: str = "linux-debug") -> bool:
    return editor_binary(repo_root, preset).is_file()


def dependencies_installed() -> bool:
    """True when the pinned dependency cache has content.

    Presence of the directory is not enough — a failed fetch leaves it existing and empty, which would make Install report
    "already installed" and then fail in CMake. Requiring a subdirectory catches that without re-implementing FetchContent's
    bookkeeping.
    """
    if not DEPENDENCY_CACHE.is_dir():
        return False
    return any(entry.is_dir() for entry in DEPENDENCY_CACHE.iterdir())


def vulkan_loader_present() -> bool:
    """True when the Vulkan userspace loader (`libvulkan.so.1`) can be found.

    The build fetches headers, volk and glslang on its own, so configure proceeds without a system Vulkan package — but it
    hard-fails later (third_party/CMakeLists.txt: `find_library(HP_VULKAN_LOADER …)` → FATAL_ERROR) and the editor cannot run
    without the loader. That check sits *after* `FetchContent_MakeAvailable`, so on an unprepared machine nothing says "no Vulkan"
    until every dependency has been downloaded and built; naming it up front is the whole point. `find_library` consults the
    loader cache/soname links rather than a fixed path, matching how the linker resolves `-lvulkan` (`.so.1` alone counts — most
    distributions ship no `libvulkan.so` symlink, which is why the engine cannot use find_package(Vulkan) either)."""
    return ctypes.util.find_library("vulkan") is not None


def editor_enabled(repo_root: Path, preset: str = "linux-debug") -> bool | None:
    """Whether the configured tree actually contains the editor module; None when it cannot be known yet.

    `HP_BUILD_EDITOR` defaults ON, but the editor is added only with `HP_ENABLE_VULKAN` too (root CMakeLists.txt), so a tree
    configured `-DHP_ENABLE_VULKAN=OFF` configures cleanly and then has no `hollowpoint-editor` to build — a launcher button
    that dies with a bare OSError. The answer is read from the preset's own CMakeCache rather than mirrored here, because a
    second copy of the rule drifts from the first: an option renamed in CMake would leave this reporting a confident wrong
    answer forever. Returns None for an unconfigured tree (unknown — not "disabled"), and treats an absent cache entry as the
    option's default (ON), so a renamed option reads as enabled rather than raising a false alarm.
    """
    if not is_configured(repo_root, preset):
        return None
    cache = repo_root / PRESETS.get(preset, PRESETS["linux-debug"]) / "CMakeCache.txt"
    flags = {"HP_BUILD_EDITOR": True, "HP_ENABLE_VULKAN": True}
    try:
        text = cache.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if not separator:
            continue
        # Keys carry their type: `HP_ENABLE_VULKAN:BOOL=OFF`. Splitting on the colon is required — comparing the left of `=` against
        # the bare option name matches nothing, which made every tree (Vulkan-less included) report enabled. Caught by
        # test_vulkan_off_disables_the_editor, which was red until this line existed.
        #
        # No comment handling is written, and none is needed, for two separate reasons: a CMake help comment has no `=` so the
        # partition above already skips it, and a commented-out entry keeps its `//` through this split (`//HP_ENABLE_VULKAN:BOOL`),
        # which is not a flag name. Both halves were probed against real caches rather than assumed, and an explicit
        # `startswith("//")` guard was removed after mutation showed no test could fail with it absent — code that nothing can break
        # is only cost for the next reader. The behaviour itself stays pinned by
        # test_commented_out_entries_never_override_the_live_value.
        name = key.split(":", 1)[0].strip()
        if name in flags:
            # Cache booleans are written "ON"/"OFF"; anything unparseable leaves the default rather than guessing False.
            flags[name] = value.strip().upper() in ("ON", "TRUE", "1", "YES", "Y")
    return flags["HP_BUILD_EDITOR"] and flags["HP_ENABLE_VULKAN"]


def usage() -> str:
    """The launcher's own help text — one source for run.sh, `--help` and the docstrings.

    Kept in core (not shell) so it is testable and cannot contradict what the launcher actually accepts: an argument list
    maintained in a comment is documentation, this is behaviour."""
    return "\n".join(
        (
            "hollowpoint launcher — a front door for the repository",
            "",
            "usage:",
            "  ./run.sh              open the graphical launcher (needs PySide6)",
            "  ./run.sh --help       this text",
            "  ./run.sh --check      toolchain and build status, no window (works over ssh, without PySide6)",
            "",
            "the launcher window runs the repository's own commands (fetch dependencies, configure,",
            "build, test, open the editor on a project); it stores no state of its own, so every action",
            "here has an equivalent command line. Build directly instead:",
            "  cmake --preset linux-debug && cmake --build build/debug -j && ctest --test-dir build/debug",
        )
    )


class TaskRunner:
    """Runs one command at a time, streaming output to a callback off the UI thread.

    Why a thread and a queue rather than a blocking call: a build takes minutes, and a GUI that blocks for minutes is
    indistinguishable from a crash. The consumer (the Qt layer, or a test) drains `poll_output`; the worker never touches a
    widget, which is the rule Qt enforces by crashing when you break it.

    One task at a time is a correctness constraint, not politeness: two concurrent `cmake --build` invocations on one tree
    race on the same object files and produce failures that look like compiler bugs.
    """

    def __init__(self) -> None:
        self._queue: queue.Queue[str | None] = queue.Queue()
        self._process: subprocess.Popen[bytes] | None = None
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()

    @property
    def is_running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self, command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
        """Starts `command`. Raises RuntimeError when a task is already running (see the concurrency note above)."""
        with self._lock:
            if self.is_running:
                raise RuntimeError("a task is already running; wait for it or press Stop")

            self._thread = threading.Thread(
                target=self._worker, args=(command, cwd, env), name="hp-launcher-task", daemon=True
            )
            self._thread.start()

    def _worker(self, command: list[str], cwd: Path, env: dict[str, str] | None) -> None:
        # Text mode with line buffering so output appears as it is produced; `2>&1` merges stderr because a compiler
        # diagnostic arriving out of order relative to the command that caused it makes a failure harder to read.
        merged_env = dict(os.environ, **(env or {}))
        try:
            process = subprocess.Popen(  # noqa: S603 - argv is composed by this module, never from a shell string
                command,
                cwd=str(cwd),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=merged_env,
            )
        except OSError as error:
            # A missing binary is reported the same way a failing one is: as output plus a non-zero exit code. Raising here
            # would surface on the worker thread, where nothing can see it.
            self._queue.put(f"could not start: {' '.join(command)}\n{error}\n")
            self._queue.put(f"[exit 127] could not start {command[0]}\n")
            return

        with self._lock:
            self._process = process

        try:
            assert process.stdout is not None
            for line in process.stdout:
                self._queue.put(line)

            exit_code = process.wait()
            self._queue.put(f"\n[exit {exit_code}] {' '.join(command)}\n")
        finally:
            # Closing the pipe explicitly rather than letting GC do it: a launcher that runs a dozen builds leaves a dozen
            # inherited descriptors open until collection, and the test suite (which runs with warnings as interesting)
            # caught exactly this. The child is reaped by wait() above on every path that got that far; cancel() paths still
            # have it, which is why close() comes before clearing the handle.
            if process.stdout is not None:
                process.stdout.close()

            with self._lock:
                self._process = None

    def poll_output(self) -> Iterator[str]:
        """Yields output lines available right now, without blocking. Empty when caught up."""
        while True:
            try:
                yield self._queue.get_nowait()
            except queue.Empty:
                return

    def cancel(self) -> bool:
        """Terminates the running process. Returns whether it was our own child (not a stale handle)."""
        with self._lock:
            process = self._process
        if process is None or process.poll() is not None:
            return False

        # terminate then let the worker observe EOF and report the exit code, rather than killing the group: cmake spawns
        # compilers, and SIGTERM to the parent is what unwinds them in an orderly way on POSIX.
        process.terminate()
        return True


def project_explorer_command(path: Path) -> list[str]:
    """Opens a file or folder in the platform's file browser.

    Empty when nothing appropriate exists. Launching a GUI helper through subprocess with shell=False means no quoting bugs
    with paths containing spaces — which, given that this one opens user directories, is the normal case rather than the edge
    case.
    """
    system = platform.system()
    if system == "Linux":
        for browser in ("xdg-open", "gio"):
            executable = shutil.which(browser)
            if executable is not None:
                if browser == "gio":
                    return [executable, "open", str(path)]
                return [executable, str(path)]
        return []
    if system == "Darwin":
        return ["open", str(path)]
    if system == "Windows":
        # `explorer` wants a path and returns non-zero even on success in some builds; the launcher ignores its exit code.
        return ["explorer", str(path)]
    return []


def python_executable() -> str:
    """The interpreter running the launcher, so child tooling uses the same one."""
    return sys.executable or "python3"
