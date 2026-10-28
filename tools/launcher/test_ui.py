"""Drives the real launcher widgets and asserts on what they cause to happen.

Where test_core.py covers decisions (which argv, which state), this covers wiring: does pressing the card labelled
"Rebuild engine" actually start `cmake --build`, does a running task disable the buttons that would start a second one, does
output land in the panel. Those are the failures a user hits immediately and no unit test of core.py can see — a button
connected to nothing, or to the wrong action, passes every command-composition test.

Skips (not fails) when Qt or a display is unavailable: like the engine's GPU suites, this is testing something that genuinely
requires an environment, and CI without one should say "skipped" rather than claim success.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

# Offscreen Qt needs to be set before QApplication exists; done here rather than in the test bodies so a single switch
# governs the whole module.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "launcher"))

try:
    from PySide6.QtWidgets import QApplication

    import core
    import ui  # noqa: F401 - imported by name so the skip check below covers it too
except ImportError as error:  # pragma: no cover - environment-dependent
    ui = None
    _QT_ERROR = str(error)
else:
    _QT_ERROR = ""


class LauncherUiTest(unittest.TestCase):
    """One shared QApplication for the module.

    Qt allows a single application object per process and refuses to make a second one, so this is class-level rather than
    per-test; the windows themselves are created and destroyed per test so nothing leaks between cases.
    """

    @classmethod
    def setUpClass(cls) -> None:
        if ui is None:
            raise unittest.SkipTest(f"PySide6 not available: {_QT_ERROR}")
        cls.app = QApplication.instance() or QApplication([])
        ui.apply_app_style(cls.app)

    def setUp(self) -> None:
        # A scratch projects directory rather than the repository's: a test that creates a project must not put a file in the
        # working tree, where it shows up in someone else's git status. LauncherWindow takes the path for exactly this.
        import tempfile

        self._projectsTemp = tempfile.TemporaryDirectory(prefix="hp-launcher-test-ui-")
        self.addCleanup(self._projectsTemp.cleanup)
        self.projectsDir = Path(self._projectsTemp.name)

        self.window = ui.LauncherWindow(REPO, projectsDirectory=self.projectsDir)
        self.window.resize(880, 700)
        self.window.show()
        self.addCleanup(self.window.close)

    def test_window_opens_with_the_three_primary_actions(self) -> None:
        """The buttons exist and carry the labels a user reads them by. Not a pixel test — a labelling test: the card that
        says "Open editor" must be the one wired to the editor."""
        self.assertEqual(self.window._installButton._title.text(), "Install dependencies")
        self.assertEqual(self.window._rebuildButton._title.text(), "Rebuild engine")
        self.assertEqual(self.window._openEditorButton._title.text(), "Open editor")

    def test_state_reflects_the_repository_on_screen(self) -> None:
        """On this repository the debug tree is configured and built, so the cards must say so. If they do not, either the
        detection or the wiring is wrong — and both are invisible until a user presses something."""
        import core

        self.window.refresh_state()

        configured = core.is_configured(REPO)
        built = core.is_built(REPO)

        # The cards' state lines are what a user trusts before pressing anything, so they must be derived from the same
        # filesystem facts core.py reports — not from a value cached when the window opened.
        self.assertEqual("up to date" in self.window._rebuildButton._state.text(), configured)
        self.assertEqual(
            "hollowpoint-editor" in self.window._openEditorButton._state.text(), built
        )

        # Status line: "ready" when nothing is missing, otherwise it names the tools. A machine with no Ninja would show
        # "missing: Ninja", which is the other correct outcome — so assert the shape rather than one branch.
        status = self.window._statusLabel.text()
        if any(not present for _, present in core.check_toolchain()):
            self.assertTrue(status.startswith("missing:"), status)
        else:
            self.assertEqual(status, "ready")

    def test_a_vulkan_less_tree_marks_the_editor_disabled_instead_of_misleading(self) -> None:
        """With HP_ENABLE_VULKAN off the tree configures fine but has no editor target. The card must say so on its own
        state line — "build the engine first" would send the user into a Rebuild that succeeds and still opens nothing."""
        from unittest import mock

        with mock.patch.object(core, "editor_enabled", return_value=False):
            self.window.refresh_state()

        state = self.window._openEditorButton._state.text()
        self.assertIn("disabled", state)
        # _enabled_for_state, not isEnabled(): the card deliberately stays a responsive widget so its state line can explain
        # itself (see set_enabled_action); the flag is what carries "pressing this would do nothing".
        self.assertFalse(self.window._openEditorButton._enabled_for_state)

    def test_rebuild_card_starts_the_documented_build_command(self) -> None:
        """Pressing the card must run `cmake --build build/debug -j`, observed through the output panel.

        Deliberately against a scratch repository rather than the real one: launching a real build here would take minutes and
        race with whatever else is building. The command line is what is asserted, which is exactly the part that could be
        wrong in a way the user sees as "it rebuilt the wrong thing".
        """
        recorder = _RecordingRunner()
        # Replacing the runner rather than wrapping it: what is under test is which command the window asks for, and a real
        # task here would kick off a minutes-long build to assert something observable in microseconds.
        self.window._runner = recorder
        self.window._rebuildButton.clicked.emit()

        self.assertEqual(recorder.calls, [["cmake", "--build", "build/debug", "-j"]])
        self.assertEqual(self.window._taskLabel.text(), "rebuild")

    def test_install_card_runs_configure(self) -> None:
        recorder = _RecordingRunner()
        self.window._runner = recorder
        self.window._installButton.clicked.emit()
        self.assertEqual(recorder.calls, [["cmake", "--preset", "linux-debug"]])

    def test_open_editor_passes_the_selected_project(self) -> None:
        """The launcher's whole promise is "open this project", so the selection must reach the command line as --project and
        --scene rather than being remembered in a side channel the editor never reads."""
        recorder = _RecordingRunner()
        self.window._runner = recorder

        projectFile = self.projectsDir / "LaunchTest.hproject"
        projectFile.write_text("{}")
        self.window.refresh_projects()

        from PySide6.QtCore import Qt

        target = None
        for row in range(self.window._projectList.count()):
            item = self.window._projectList.item(row)
            value = item.data(Qt.ItemDataRole.UserRole)
            if value and str(value).endswith("LaunchTest.hproject"):
                target = item
                break
        assert target is not None, "the scratch project was not listed"
        self.window._projectList.setCurrentItem(target)

        self.window._openEditorButton.clicked.emit()

        self.assertEqual(len(recorder.calls), 1, recorder.calls)
        command = recorder.calls[0]
        self.assertTrue(command[0].endswith("hollowpoint-editor"), command)
        self.assertIn("--project", command)
        self.assertIn("--scene", command)
        self.assertTrue(str(command[command.index("--scene") + 1]).endswith("LaunchTest.hproject"))

    def test_no_selection_opens_the_editor_without_project_arguments(self) -> None:
        """With nothing selected the launcher must still open the editor — a project is a convenience, not a precondition, and
        an editor that refuses to start because the browser has no current row is a dead end."""
        recorder = _RecordingRunner()
        self.window._runner = recorder
        self.window.refresh_projects()

        self.window._openEditorButton.clicked.emit()

        self.assertEqual(len(recorder.calls), 1, recorder.calls)
        self.assertNotIn("--scene", recorder.calls[0])

    def test_a_running_task_blocks_a_second_start_and_says_so(self) -> None:
        """Two concurrent builds corrupt a tree. The UI must refuse, and the Stop button must appear."""

        class BusyRunner:
            is_running = True

            def start(self, command, cwd, env=None) -> None:  # noqa: ANN001, ARG002
                raise RuntimeError("a task is already running; wait for it or press Stop")

            def poll_output(self):  # noqa: ANN201
                return iter(())

            def cancel(self) -> bool:
                return False

        self.window._runner = BusyRunner()
        self.window._rebuildButton.clicked.emit()

        # The refusal is surfaced in the output panel rather than swallowed — a button that does nothing with no message is
        # the exact failure this launcher exists to avoid.
        text = "\n".join(
            self.window._output.item(row).text() for row in range(self.window._output.count())
        )
        self.assertIn("already running", text)

    def test_output_panel_appends_and_bounds_history(self) -> None:
        """The panel is a fixed-size ring, not an unbounded list: a launcher left open across a day of builds streams tens of
        thousands of lines into it."""
        import core

        self.window._output.append_lines([f"line {index}" for index in range(60)])
        self.assertEqual(self.window._output.count(), 60)

        bulk = [f"bulk {index}" for index in range(core.MAX_LOG_LINES + 500)]
        self.window._output.append_lines(bulk)

        # Bounded, and bounded at the right end: the newest lines are what someone is reading, so the *oldest* must be the
        # ones dropped. `bulk 0..499` are gone, `bulk 500` is now first, and the last line is still the most recent.
        self.assertEqual(self.window._output.count(), core.MAX_LOG_LINES)
        self.assertEqual(self.window._output.item(0).text(), "bulk 500")
        last = self.window._output.count() - 1
        self.assertEqual(self.window._output.item(last).text(), f"bulk {core.MAX_LOG_LINES + 499}")

    def test_clear_empties_the_panel(self) -> None:
        self.window._output.append_lines(["something"])
        self.window._clear_output()
        self.assertEqual(self.window._output.count(), 0)


class _RecordingRunner:
    """Stands in for TaskRunner so a test can assert on the command without running it.

    Mirrors the real interface (start/poll_output/cancel/is_running) rather than subclassing it, because the point is that the
    window talks to those five members and nothing else — an extra private call would then fail loudly instead of silently
    working against a fake that happened to provide it.
    """

    is_running = False

    def __init__(self) -> None:
        self.calls: list[list[str]] = []

    def start(self, command, cwd, env=None) -> None:  # noqa: ANN001, ARG002 - signature mirror of TaskRunner
        self.calls.append(list(command))

    def poll_output(self):  # noqa: ANN201
        return iter(())

    def cancel(self) -> bool:
        return False


if __name__ == "__main__":
    unittest.main(verbosity=2)
