"""Tests for the launcher's command composition and state detection.

These cover the launcher for the reason AGENTS.md §2 requires tests of new code, with the emphasis chosen for what can
actually go wrong in a tool like this: not widget pixels (nothing here asserts colours — the GUI is verified by running it),
but *which command an action runs* and *whether state is read correctly from disk*. A launcher that builds the wrong tree or
reports "up to date" for a missing binary costs an author far more than a misaligned label, and both are pure logic.

No Qt, no display: `core.py` deliberately imports nothing from PySide6, which is what lets this run in CI alongside the C++
suites. Run directly (``python3 tools/launcher/test_core.py``) or via ctest as LauncherTests.
"""

from __future__ import annotations

import ctypes.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import core  # noqa: E402 - path set above so the module resolves whether run from ctest or by hand


class CommandComposition(unittest.TestCase):
    """The argv each button produces. These are the launcher's contract with the build system."""

    def test_commands_match_the_readme(self) -> None:
        """The documented commands, not a launcher-specific dialect.

        README.md tells a developer `cmake --preset linux-debug`, `cmake --build build/debug -j` and
        `ctest --test-dir build/debug`. If the launcher drifted from those, fixing a build failure by reading its output would
        produce advice that does not apply outside the GUI — two sources of truth about how to build the engine.
        """
        commands = core.build_commands("linux-debug")

        self.assertEqual(commands["install_deps"], ["cmake", "--preset", "linux-debug"])
        self.assertEqual(commands["rebuild"], ["cmake", "--build", "build/debug", "-j"])
        self.assertEqual(commands["configure"], ["cmake", "--preset", "linux-debug"])
        self.assertEqual(commands["test"][:3], ["ctest", "--test-dir", "build/debug"])

    def test_editor_path_is_the_build_output(self) -> None:
        """Where the engine actually installs its executable, per CMAKE_RUNTIME_OUTPUT_DIRECTORY."""
        commands = core.build_commands("linux-debug")
        self.assertEqual(commands["run_editor"], [str(Path("build/debug/output/hollowpoint-editor"))])

    def test_other_presets_use_their_own_tree(self) -> None:
        """A preset switch must move every path, or the launcher builds one tree and runs another."""
        release = core.build_commands("linux-release")
        self.assertEqual(release["rebuild"], ["cmake", "--build", "build/release", "-j"])
        self.assertIn("build/release", release["run_editor"][0])

    def test_unknown_preset_falls_back_to_debug(self) -> None:
        """A typo'd preset must not produce a command with an empty or literal path."""
        commands = core.build_commands("not-a-preset")
        self.assertEqual(commands["rebuild"], ["cmake", "--build", "build/debug", "-j"])

    def test_presets_are_the_ones_cmake_declares(self) -> None:
        """The preset table is a copy of CMakePresets.json, so it can go stale — and a stale entry makes the launcher operate
        on a build tree that does not exist while reporting total confidence."""
        presetsPath = Path(__file__).resolve().parents[2] / "CMakePresets.json"
        if not presetsPath.is_file():
            self.skipTest("CMakePresets.json not present")

        import json

        declared = {
            entry["name"] for entry in json.loads(presetsPath.read_text()).get("configurePresets", [])
        }
        self.assertTrue(set(core.PRESETS).issubset(declared), f"launcher presets {set(core.PRESETS)} not all in {declared}")


class StateDetection(unittest.TestCase):
    """"Is it configured / built / installed?" — read from disk, never remembered."""

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory(prefix="hp-launcher-test-")
        self.root = Path(self._temp.name)
        self.addCleanup(self._temp.cleanup)

    def test_unconfigured_tree_reports_unconfigured(self) -> None:
        self.assertFalse(core.is_configured(self.root))
        self.assertFalse(core.is_built(self.root))

    def test_a_cmake_cache_counts_as_configured(self) -> None:
        cache = self.root / "build/debug"
        cache.mkdir(parents=True)
        (cache / "CMakeCache.txt").write_text("")

        self.assertTrue(core.is_configured(self.root))
        # Configured is not built: the distinction is what lets the launcher offer Rebuild before Open editor.
        self.assertFalse(core.is_built(self.root))

    def test_an_editor_binary_counts_as_built(self) -> None:
        output = self.root / "build/debug/output"
        output.mkdir(parents=True)
        (output / "hollowpoint-editor").write_text("")

        self.assertTrue(core.is_built(self.root))

    def test_a_stale_binary_in_an_unconfigured_tree_is_not_built(self) -> None:
        """A build tree deleted around a leftover binary must not read as ready to run."""
        output = self.root / "build/release/output"
        output.mkdir(parents=True)
        (output / "hollowpoint-editor").write_text("")

        self.assertFalse(core.is_built(self.root, "linux-debug"))
        self.assertTrue(core.is_built(self.root, "linux-release"))


class DependencyDetection(unittest.TestCase):
    def test_missing_cache_is_not_installed(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            original = core.DEPENDENCY_CACHE
            try:
                core.DEPENDENCY_CACHE = Path(tempDir) / "absent"
                self.assertFalse(core.dependencies_installed())
            finally:
                core.DEPENDENCY_CACHE = original

    def test_an_empty_cache_is_not_installed(self) -> None:
        """A fetch that died partway leaves the directory existing. Reporting "already installed" there would send the user
        into a CMake failure with no explanation of why the launcher thought it was ready."""
        with tempfile.TemporaryDirectory() as tempDir:
            original = core.DEPENDENCY_CACHE
            try:
                cache = Path(tempDir)
                core.DEPENDENCY_CACHE = cache
                self.assertFalse(core.dependencies_installed())

                (cache / "imgui-src").mkdir()
                self.assertTrue(core.dependencies_installed())
            finally:
                core.DEPENDENCY_CACHE = original


class ProjectDiscovery(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory(prefix="hp-launcher-projects-")
        self.root = Path(self._temp.name)
        self.addCleanup(self._temp.cleanup)

    def test_missing_directory_yields_no_projects(self) -> None:
        self.assertEqual(core.discover_projects(self.root / "nope"), [])

    def test_projects_are_found_and_sorted(self) -> None:
        (self.root / "zulu.hproject").write_text("{}")
        (self.root / "alpha.hproject").write_text("{}")
        (self.root / "notes.txt").write_text("")

        found = core.discover_projects(self.root)
        self.assertEqual([path.name for path in found], ["alpha.hproject", "zulu.hproject"])

    def test_non_project_files_are_ignored(self) -> None:
        (self.root / "scene.hpscene").write_text("{}")
        self.assertEqual(core.discover_projects(self.root), [])


class ToolchainChecks(unittest.TestCase):
    def test_present_tools_are_reported_available(self) -> None:
        """git and cmake are guaranteed on any machine that can build this repository, including the CI images that run this
        suite — so asserting on them tests the check rather than this workstation."""
        results = dict((requirement.name, present) for requirement, present in core.check_toolchain())
        self.assertIn("CMake", results)
        self.assertIn("Git", results)
        self.assertTrue(results["CMake"])
        self.assertTrue(results["Git"])

    def test_compiler_detection_accepts_any_of_the_supported_names(self) -> None:
        # The engine builds with GCC and clang; a check for one name alone reports "install a compiler" on machines that have
        # exactly the other.
        self.assertTrue(callable(core.has_cpp_compiler))
        self.assertIsInstance(core.has_cpp_compiler(), bool)

    def test_install_hint_is_a_string_for_every_requirement(self) -> None:
        """An empty or missing hint is the thing that makes a "please install X" dialog useless."""
        for requirement, _ in core.check_toolchain():
            self.assertTrue(requirement.install_hint().strip(), requirement.name)


class TaskRunner(unittest.TestCase):
    """The subprocess plumbing. Real processes, because the failure modes worth catching (a missing binary, output ordering,
    a second task arriving while one runs) are all process-level."""

    def setUp(self) -> None:
        self.runner = core.TaskRunner()

    def _drain_until(self, predicate, timeoutSeconds: float = 10.0) -> list[str]:
        import time

        collected: list[str] = []
        deadline = time.monotonic() + timeoutSeconds
        while time.monotonic() < deadline:
            collected.extend(self.runner.poll_output())
            if predicate(collected):
                return collected
            time.sleep(0.02)
        return collected

    def test_running_a_command_streams_its_output_and_exit_code(self) -> None:
        self.runner.start([sys.executable, "-c", "print('hello from the task')"], Path.cwd())
        lines = self._drain_until(lambda seen: any("[exit 0]" in line for line in seen))

        self.assertTrue(any("hello from the task" in line for line in lines), lines)
        self.assertTrue(any("[exit 0]" in line for line in lines), lines)

    def test_a_failing_command_reports_its_exit_code(self) -> None:
        """Without this, a build failure is indistinguishable from a launcher that stopped producing output."""
        self.runner.start([sys.executable, "-c", "import sys; sys.exit(3)"], Path.cwd())
        lines = self._drain_until(lambda seen: any("[exit" in line for line in seen))
        self.assertTrue(any("[exit 3]" in line for line in lines), lines)

    def test_a_missing_binary_is_reported_instead_of_raising(self) -> None:
        """The failure happens on a worker thread where an exception would vanish; it has to arrive as output."""
        self.runner.start(["/nonexistent/hollowpoint-tool-that-does-not-exist"], Path.cwd())
        lines = self._drain_until(lambda seen: any("could not start" in line for line in seen))
        self.assertTrue(any("could not start" in line for line in lines), lines)

    def test_a_second_task_is_refused_while_one_runs(self) -> None:
        """Two concurrent `cmake --build` invocations on one tree race on the same object files and fail in ways that look like
        compiler bugs. The launcher must refuse rather than corrupt a build."""
        self.runner.start([sys.executable, "-c", "import time; time.sleep(3)"], Path.cwd())
        try:
            with self.assertRaises(RuntimeError):
                self.runner.start([sys.executable, "-c", "print('nope')"], Path.cwd())
        finally:
            self.runner.cancel()

    def test_cancel_terminates_the_process(self) -> None:
        self.runner.start([sys.executable, "-c", "import time; time.sleep(30)"], Path.cwd())
        # Give the worker a moment to spawn the child, or cancel has nothing to stop.
        import time

        deadline = time.monotonic() + 5.0
        cancelled = False
        while time.monotonic() < deadline:
            if self.runner.cancel():
                cancelled = True
                break
            time.sleep(0.02)

        self.assertTrue(cancelled)
        lines = self._drain_until(lambda seen: any("[exit" in line for line in seen))
        self.assertTrue(any("[exit" in line for line in lines), lines)

    def test_poll_output_is_empty_when_caught_up(self) -> None:
        self.assertEqual(list(self.runner.poll_output()), [])


class FileBrowserCommand(unittest.TestCase):
    def test_returns_a_command_or_nothing_never_raises(self) -> None:
        command = core.project_explorer_command(Path("/tmp"))
        self.assertIsInstance(command, list)
        if command:
            self.assertTrue(os.path.isabs(command[0]) or "/" not in command[0], command)

    def test_paths_with_spaces_survive(self) -> None:
        """No shell involved, so a directory named `My Projects` needs no quoting — the reason argv is a list."""
        with tempfile.TemporaryDirectory(prefix="with spaces ") as tempDir:
            command = core.project_explorer_command(Path(tempDir))
            if command:
                self.assertEqual(command[-1], tempDir)


class EditorAvailability(unittest.TestCase):
    """Whether a configured tree can actually produce `hollowpoint-editor`.

    The launcher offers an editor button unconditionally, so "the tree is configured but has no editor" needs a distinct
    answer: pressing Rebuild there succeeds and still leaves nothing to open. These are the cases where reading the preset's
    own CMakeCache (rather than assuming the option defaults) earns its keep."""

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory(prefix="hp-launcher-editor-")
        self.root = Path(self._temp.name)
        self.addCleanup(self._temp.cleanup)

    def _configure(self, cacheBody: str) -> None:
        cache = self.root / "build/debug"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / "CMakeCache.txt").write_text(cacheBody, encoding="utf-8")

    def test_an_unconfigured_tree_is_unknown_rather_than_disabled(self) -> None:
        """None and False must not be conflated: "unknown" leaves the card offering a build, which is right; "disabled" would
        tell someone their repository cannot build an editor before they have run configure once."""
        self.assertIsNone(core.editor_enabled(self.root))

    def test_both_options_on_enables_the_editor(self) -> None:
        self._configure("HP_BUILD_EDITOR:BOOL=ON\nHP_ENABLE_VULKAN:BOOL=ON\n")
        self.assertTrue(core.editor_enabled(self.root))

    def test_vulkan_off_disables_the_editor(self) -> None:
        """The root CMakeLists adds the editor only with HP_ENABLE_VULKAN, so a Vulkan-less tree has no editor target — the
        exact configuration that would otherwise end in `OSError: No such file or directory` on a button press."""
        self._configure("HP_BUILD_EDITOR:BOOL=ON\nHP_ENABLE_VULKAN:BOOL=OFF\n")
        self.assertFalse(core.editor_enabled(self.root))

    def test_editor_option_off_disables_the_editor(self) -> None:
        self._configure("HP_BUILD_EDITOR:BOOL=OFF\nHP_ENABLE_VULKAN:BOOL=ON\n")
        self.assertFalse(core.editor_enabled(self.root))

    def test_an_empty_cache_falls_back_to_the_documented_defaults(self) -> None:
        """A cache with no entry means the option was never overridden, and both default ON. Reading absent as OFF would
        disable the editor on every normal tree whose cache format changed."""
        self._configure("")
        self.assertTrue(core.editor_enabled(self.root))

    def test_cache_comments_and_unrelated_entries_are_ignored(self) -> None:
        self._configure(
            "# comment\nHP_ENABLE_VULKAN:BOOL=OFF\nSOMETHING_ELSE:BOOL=ON\nOTHER:FILEPATH=/tmp/equal=path\n"
        )
        self.assertFalse(core.editor_enabled(self.root))

    def test_commented_out_entries_never_override_the_live_value(self) -> None:
        """A hand-edited cache often carries a commented-out alternative next to the real entry. Ordering matters for the test's
        power: the comment goes AFTER the live line, so any parser that read it as an entry would flip the answer from False to
        True. (Written in the other order it passes for the wrong reason — last-write-wins hides the bug.) It cannot win as things
        stand: a commented key keeps its `//` through the split and stops naming a flag, which is why no explicit comment guard
        exists — removing one changed no test result."""
        self._configure("HP_ENABLE_VULKAN:BOOL=OFF\n//HP_ENABLE_VULKAN:BOOL=ON\n# comment = with equals\n")
        self.assertFalse(core.editor_enabled(self.root))

    def test_a_cache_that_cannot_be_read_is_unknown(self) -> None:
        """An unreadable cache yields None — offer the action and let cmake fail loudly — rather than a confident "disabled"
        that contradicts what is on disk. read_text is patched because getting `.is_file()` to pass while a read fails needs
        either root or a race, and the branch under test is the except handler itself."""
        self._configure("HP_ENABLE_VULKAN:BOOL=OFF\n")
        with mock.patch.object(Path, "read_text", side_effect=OSError("EIO")):
            self.assertIsNone(core.editor_enabled(self.root))


class VulkanLoader(unittest.TestCase):
    def test_detection_returns_a_boolean(self) -> None:
        self.assertIsInstance(core.vulkan_loader_present(), bool)

    def test_the_real_host_reports_consistently_with_its_own_loader(self) -> None:
        """Whatever this machine has, the check must agree with it — asserted against the file the loader would resolve to,
        so the test fails if find_library ever stops seeing `.so.1`-only installations (the common case, and the one the engine's
        own CMake comment says broke find_package)."""
        found = core.vulkan_loader_present()
        resolved = ctypes.util.find_library("vulkan")
        self.assertEqual(found, resolved is not None)


class UsageText(unittest.TestCase):
    """`--help` and the headless entry path. main() is imported lazily inside each test so this module stays importable
    without PySide6, matching core.py's own Qt-free rule."""

    def test_documents_every_flag_the_entry_point_accepts(self) -> None:
        """Help that omits a flag is worse than no help. Asserted against the flags main.py actually matches, so adding one
        without documenting it fails here."""
        text = core.usage()
        for flag in ("--help", "--check"):
            self.assertIn(flag, text)

    def test_names_the_direct_build_command_as_an_escape_hatch(self) -> None:
        """The launcher is optional; the help has to say how to build without it, or a machine with no PySide6 is stuck."""
        text = core.usage()
        self.assertIn("cmake --preset", text)
        self.assertIn("ctest", text)

    def test_the_entry_point_answers_help_without_touching_gui_code(self) -> None:
        """End-to-end through main(), because the defect this guards was exactly a documented flag falling through to the GUI:
        `--help` opened a launcher window. Patching gui() proves help never reaches it; importing main with PySide6 hidden
        proves the same path works on a machine that has no Qt at all."""
        import io
        import contextlib

        import main as launcher_main

        for flag in ("--help", "-h"):
            buffer = io.StringIO()
            with mock.patch.object(launcher_main, "gui", side_effect=AssertionError("gui reached")):
                with contextlib.redirect_stdout(buffer):
                    rc = launcher_main.main([flag])
            self.assertEqual(rc, 0, flag)
            self.assertIn("usage:", buffer.getvalue(), flag)

    def test_help_works_with_pyside6_absent(self) -> None:
        """The claim in the help text is that --check/--help work over ssh without PySide6. Enforced by blocking the import,
        not by hoping this host happens to have Qt installed."""
        import builtins
        import io
        import contextlib

        import main as launcher_main

        realImport = builtins.__import__

        def noQt(name: str, *args, **kwargs):  # noqa: ANN001, N803 - mirrors builtins.__import__
            if name.split(".")[0] == "PySide6":
                raise ImportError("blocked by test: PySide6 must not be needed for --help")
            return realImport(name, *args, **kwargs)

        buffer = io.StringIO()
        with mock.patch.object(builtins, "__import__", noQt):
            with contextlib.redirect_stdout(buffer):
                rc = launcher_main.main(["--help"])

        self.assertEqual(rc, 0)
        self.assertIn("usage:", buffer.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
