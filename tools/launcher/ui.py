"""Hollowpoint launcher UI — a glass-bubble front door over the repository's own commands.

The constraints that shaped this file, in the order they constrained:

* **Every action is also available without it.** The buttons call exactly the commands README.md documents (composed in
  `core.py`), so nothing here is the only way to do something — AGENTS.md §9's rule for editor features, applied to tooling.
* **State is read, not remembered.** "Is it built?" asks the filesystem; "is it configured?" asks for a CMakeCache. A launcher
  caching those answers in a label would show a green tick beside a tree someone just deleted by hand, and an author stops
  trusting a launcher the day it does that.
* **Output is never swallowed.** Pressing a button streams the real command's output into the panel below. The failure mode this
  exists to prevent is the GUI that reports "Build failed" and nothing else, sending the user to a terminal to learn what
  happened — at which point it is an obstacle rather than a tool.

All repository knowledge lives in `core.py`; this module only composes widgets from it, which is what lets the command
composition be tested without Qt or a display (see tools/launcher/test_core.py).
"""

from __future__ import annotations

import functools
import subprocess
from pathlib import Path

from PySide6.QtCore import QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen, QRadialGradient
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

import core

# Palette. A dark translucent base with one accent, because a launcher sits on top of an arbitrary desktop and has to read
# against both a bright wallpaper and a black one — the glass blur adapts, so the colours themselves stay fixed.
ACCENT = QColor(120, 200, 255)
TEXT = QColor(232, 238, 246)
TEXT_DIM = QColor(150, 162, 178)
DANGER = QColor(255, 130, 130)
OK = QColor(140, 230, 170)
WARN = QColor(240, 210, 130)

PANEL_STYLE = "rgba(24, 28, 36, 210)"
CARD_STYLE = "rgba(40, 46, 58, 190)"
CARD_HOVER = "rgba(56, 64, 80, 210)"

# How often the window re-reads build state from disk. Fast enough to feel live after a build finishes, slow enough that the
# handful of stat() calls it makes never shows up next to the frames it costs to repaint.
REFRESH_INTERVAL_MS = 700

class BubbleButton(QFrame):
    """One glass action card: title, one-line description, and a state line that says what will actually happen.

    A QFrame rather than a QPushButton because the pressed look, the hover glow and the disabled dimming all have to agree with
    the translucent styling — with a QPushButton, Qt's own style draws an opaque background over any translucency set through a
    stylesheet, which is exactly the flat grey box this design has to avoid.
    """

    clicked = Signal()

    def __init__(self, title: str, description: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._hovered = False
        self._pressed = False
        self._enabled_for_state = True
        self._accent = ACCENT

        self._title = QLabel(title, self)
        self._title.setFont(QFont("Inter", 12, QFont.Weight.DemiBold))
        self._title.setStyleSheet(f"color: {TEXT.name()}; background: transparent;")

        self._description = QLabel(description, self)
        self._description.setWordWrap(True)
        self._description.setStyleSheet(f"color: {TEXT_DIM.name()}; background: transparent;")

        self._state = QLabel("", self)
        self._state.setStyleSheet(f"color: {ACCENT.name()}; background: transparent;")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(3)
        layout.addWidget(self._title)
        layout.addWidget(self._description)
        layout.addWidget(self._state)

        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumHeight(84)

    def set_state(self, text: str, colour: QColor = ACCENT) -> None:
        """Sets the third line — what will happen if pressed right now, or why pressing does nothing."""
        self._state.setText(text)
        self._state.setStyleSheet(f"color: {colour.name()}; background: transparent;")
        self._accent = colour

    def set_enabled_action(self, enabled: bool) -> None:
        # Not setEnabled(): a disabled QFrame stops receiving mouse events, so the card would look dead and give no reason.
        # Keeping it responsive lets the state line explain what is missing, which is the difference between a tool and a wall.
        self._enabled_for_state = enabled
        self.update()

    def enterEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self._hovered = True
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self._hovered = False
        self._pressed = False
        self.update()
        super().leaveEvent(event)

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if event.button() == Qt.MouseButton.LeftButton and self._enabled_for_state:
            self._pressed = True
            self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if event.button() != Qt.MouseButton.LeftButton:
            return
        wasPressed = self._pressed
        self._pressed = False
        self.update()
        if wasPressed and self._enabled_for_state and self.rect().contains(event.position().toPoint()):
            self.clicked.emit()

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        radius = 14.0
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        path = QPainterPath()
        path.addRoundedRect(rect, radius, radius)

        base = QColor(CARD_HOVER if self._hovered and self._enabled_for_state else CARD_STYLE)
        if not self._enabled_for_state:
            base.setAlpha(90)
        painter.fillPath(path, base)

        # A rim in the state's colour: the eye finds the coloured cards without reading, which is what makes "Install"
        # obviously still pending after a build finishes.
        rim = QColor(self._accent)
        if not self._enabled_for_state:
            rim.setAlpha(60)
        elif self._hovered:
            rim.setAlpha(220)
        else:
            rim.setAlpha(110)
        painter.setPen(QPen(rim, 1.4 if self._pressed else 1.0))
        painter.drawPath(path)

        if self._enabled_for_state and self._hovered:
            glow = QColor(self._accent)
            glow.setAlpha(36)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(glow)
            painter.drawPath(path)

        painter.end()


class OutputPanel(QListWidget):
    """Scrolling command output with an auto-follow that stops when the user scrolls back to read."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setFont(QFont("JetBrains Mono", 9))
        self.setUniformItemSizes(True)
        self.setSelectionMode(QListWidget.SelectionMode.ContiguousSelection)
        self.setStyleSheet(
            "QListWidget { background: rgba(10, 12, 16, 200); border: 1px solid rgba(255,255,255,24);"
            " color: #d8e0ea; border-radius: 10px; padding: 6px; }"
            "QListWidget::item { padding: 0px; }"
        )
        self._follow = True
        self.verticalScrollBar().valueChanged.connect(self._on_scroll)

    def _on_scroll(self, value: int) -> None:
        # Following unconditionally makes history unreadable: the next log line yanks the view down. Engaging again at the
        # bottom is what every terminal does, and anything else feels broken within a minute of use.
        self._follow = value >= self.verticalScrollBar().maximum() - 4

    def append_lines(self, lines: list[str]) -> None:
        if not lines:
            return

        for line in lines:
            item = QListWidgetItem(line.rstrip("\n"))
            # Colour by content rather than by stream: cmake prints "-- Configuring done" and errors alike on stdout, so the
            # text itself is the only reliable signal for what should be red.
            lowered = line.lower()
            if "[exit 0]" in line:
                item.setForeground(OK)
            elif "[exit" in line or "error" in lowered or "failed" in lowered:
                item.setForeground(DANGER)
            elif "--" in line[:3] or "warning" in lowered:
                item.setForeground(QColor(240, 210, 130))
            self.addItem(item)

        # Bounded: a full failed build is thousands of lines and a launcher left open across a day of rebuilds must not grow
        # without limit to hold them. Oldest go first — the tail is what is being read.
        while self.count() > core.MAX_LOG_LINES:
            self.takeItem(0)

        if self._follow:
            self.scrollToBottom()

    def clear_output(self) -> None:
        self.clear()


class LauncherWindow(QWidget):
    """The launcher window itself: frameless with a translucent background, since the glass styling requires the compositor
    to composite — an opaque top-level would render the translucency as flat black."""

    def __init__(self, repo_root: Path, projectsDirectory: Path | None = None) -> None:
        """`projectsDirectory` defaults to `<repo>/projects`.

        Injectable rather than always derived because the alternative is a test that writes into the repository to exercise
        project discovery — which is exactly what this launcher's own tests did before, leaking a scratch .hproject into the
        working tree for another developer to find in `git status`.
        """
        super().__init__()
        self._repo = repo_root
        self._projectsDirectory = projectsDirectory if projectsDirectory is not None else repo_root / "projects"
        self._runner = core.TaskRunner()
        self._dragOffset = None

        self.setWindowTitle("Hollowpoint")
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setMinimumSize(760, 620)
        self.resize(880, 700)

        self._build_ui()

        # Output is polled rather than signalled: the worker writes to a queue (it cannot touch Qt objects from its thread),
        # and a 60 Hz drain batches naturally into one repaint per frame instead of one per line. A build can emit thousands
        # of lines a second, and appending each with its own update would paint the UI into unusability.
        self._outputTimer = QTimer(self)
        self._outputTimer.timeout.connect(self._drain_output)
        self._outputTimer.start(16)

        self._stateTimer = QTimer(self)
        self._stateTimer.timeout.connect(self.refresh_state)
        self._stateTimer.start(REFRESH_INTERVAL_MS)

        self.refresh_state()

    # --- construction -----------------------------------------------------------------

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(18, 18, 18, 18)
        root.setSpacing(14)

        root.addWidget(self._build_header())
        root.addWidget(self._build_actions(), 0)
        root.addWidget(self._build_project_list(), 1)
        root.addWidget(self._build_output(), 2)

    def _build_header(self) -> QWidget:
        header = QFrame()
        layout = QHBoxLayout(header)
        layout.setContentsMargins(4, 0, 4, 0)
        layout.setSpacing(10)

        title = QLabel("Hollowpoint")
        title.setFont(QFont("Inter", 19, QFont.Weight.Bold))
        title.setStyleSheet(f"color: {TEXT.name()}; background: transparent;")

        subtitle = QLabel(str(self._repo))
        subtitle.setStyleSheet(f"color: {TEXT_DIM.name()}; background: transparent;")

        self._statusLabel = QLabel("")
        self._statusLabel.setStyleSheet(f"color: {ACCENT.name()}; background: transparent;")

        layout.addWidget(title)
        layout.addSpacing(12)
        layout.addWidget(subtitle, 1)
        layout.addWidget(self._statusLabel, 0, Qt.AlignmentFlag.AlignRight)
        return header

    def _build_actions(self) -> QWidget:
        container = QFrame()
        layout = QHBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)

        self._installButton = BubbleButton("Install dependencies", "Fetch and build the pinned third-party sources")
        self._installButton.clicked.connect(functools.partial(self._start_task, "install_deps"))

        self._rebuildButton = BubbleButton("Rebuild engine", "Compile the configured tree, then its tests")
        self._rebuildButton.clicked.connect(functools.partial(self._start_task, "rebuild"))

        self._openEditorButton = BubbleButton("Open editor", "Launch the level editor on the selected project")
        self._openEditorButton.clicked.connect(self._open_editor)

        for button in (self._installButton, self._rebuildButton, self._openEditorButton):
            layout.addWidget(button, 1)

        return container

    def _build_project_list(self) -> QWidget:
        card = QFrame()
        layout = QVBoxLayout(card)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(8)

        heading = QHBoxLayout()
        title = QLabel("Projects")
        title.setFont(QFont("Inter", 12, QFont.Weight.DemiBold))
        title.setStyleSheet(f"color: {TEXT.name()}; background: transparent;")

        self._projectHint = QLabel("")
        self._projectHint.setStyleSheet(f"color: {TEXT_DIM.name()}; background: transparent;")

        heading.addWidget(title)
        heading.addStretch(1)
        heading.addWidget(self._projectHint)
        layout.addLayout(heading)

        self._projectList = QListWidget()
        self._projectList.setFont(QFont("Inter", 10))
        self._projectList.setAlternatingRowColors(True)
        self._projectList.setStyleSheet(
            "QListWidget { background: rgba(12, 14, 18, 170); border: 1px solid rgba(255,255,255,20);"
            " border-radius: 10px; color: #dfe6ef; }"
            "QListWidget::item { padding: 6px 8px; }"
            "QListWidget::item:selected { background: rgba(120, 200, 255, 60); }"
        )
        layout.addWidget(self._projectList, 1)

        row = QHBoxLayout()
        self._explorerButton = QPushButton("Open in file browser")
        self._explorerButton.clicked.connect(self._open_project_explorer)
        self._row_button_style(self._explorerButton)
        row.addWidget(self._explorerButton)

        self._refreshButton = QPushButton("Rescan")
        self._refreshButton.clicked.connect(self.refresh_projects)
        self._row_button_style(self._refreshButton)
        row.addWidget(self._refreshButton)
        row.addStretch(1)
        layout.addLayout(row)

        return card

    def _row_button_style(self, button: QPushButton) -> None:
        button.setCursor(Qt.CursorShape.PointingHandCursor)
        button.setStyleSheet(
            "QPushButton { background: rgba(48,56,70,200); color: #dfe6ef; border: 1px solid rgba(255,255,255,30);"
            " border-radius: 9px; padding: 7px 14px; }"
            "QPushButton:hover { background: rgba(66,78,96,220); }"
            "QPushButton:pressed { background: rgba(90,104,126,230); }"
        )

    def _build_output(self) -> QWidget:
        card = QFrame()
        layout = QVBoxLayout(card)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(8)

        heading = QHBoxLayout()
        title = QLabel("Output")
        title.setFont(QFont("Inter", 12, QFont.Weight.DemiBold))
        title.setStyleSheet(f"color: {TEXT.name()}; background: transparent;")

        self._taskLabel = QLabel("idle")
        self._taskLabel.setStyleSheet(f"color: {TEXT_DIM.name()}; background: transparent;")

        self._stopButton = QPushButton("Stop")
        self._stopButton.setEnabled(False)
        self._stopButton.clicked.connect(self._stop_task)
        self._row_button_style(self._stopButton)

        clear = QPushButton("Clear")
        clear.clicked.connect(self._clear_output)
        self._row_button_style(clear)

        heading.addWidget(title)
        heading.addSpacing(10)
        heading.addWidget(self._taskLabel, 1)
        heading.addWidget(clear)
        heading.addWidget(self._stopButton)
        layout.addLayout(heading)

        self._output = OutputPanel()
        layout.addWidget(self._output, 1)

        return card

    # --- state ------------------------------------------------------------------------

    def refresh_state(self) -> None:
        """Re-reads the filesystem and updates every affordance. Cheap enough to run on a timer, which is what keeps it true."""
        configured = core.is_configured(self._repo)
        built = core.is_built(self._repo)
        deps = core.dependencies_installed()

        self._installButton.set_state(
            "already installed" if deps else f"→ {core.DEPENDENCY_CACHE}",
            OK if deps else ACCENT,
        )
        # Install is still pressable when the cache exists: a partially-failed fetch leaves it present but incomplete, and
        # re-running configure is how that heals. Reporting "installed" without allowing the repair would strand the user.

        self._rebuildButton.set_enabled_action(configured)
        self._rebuildButton.set_state(
            "up to date" if built else "not configured yet",
            ACCENT if configured else QColor(240, 210, 130),
        )

        editorAvailable = core.editor_enabled(self._repo)
        # Three states, not two: "build the engine first", "this tree cannot build an editor at all", and ready. Collapsing
        # the middle one into "build first" sends the user to press Rebuild, which succeeds and still leaves no editor — the
        # dead end is in their configure flags, so that is where the message has to point.
        if editorAvailable is False:
            self._openEditorButton.set_enabled_action(False)
            self._openEditorButton.set_state("disabled in this build", WARN)
        else:
            self._openEditorButton.set_enabled_action(built and not self._runner.is_running)
            self._openEditorButton.set_state(
                str(core.editor_binary(self._repo).name) if built else "build the engine first",
                ACCENT if built else QColor(240, 210, 130),
            )

        missing = [requirement.name for requirement, present in core.check_toolchain() if not present]
        if missing:
            self._statusLabel.setText("missing: " + ", ".join(missing))
            self._statusLabel.setStyleSheet(f"color: {DANGER.name()}; background: transparent;")
        elif self._runner.is_running:
            self._statusLabel.setText("working…")
            self._statusLabel.setStyleSheet(f"color: {ACCENT.name()}; background: transparent;")
        else:
            self._statusLabel.setText("ready")
            self._statusLabel.setStyleSheet(f"color: {OK.name()}; background: transparent;")

    def refresh_projects(self) -> None:
        self._projectList.clear()
        projects = core.discover_projects(self._projectsDirectory)
        for path in projects:
            item = QListWidgetItem(path.stem)
            item.setData(Qt.ItemDataRole.UserRole, str(path))
            item.setToolTip(str(path))
            self._projectList.addItem(item)

        self._projectHint.setText(
            f"{len(projects)} found" if projects else "no .hproject files under projects/"
        )

    # --- actions ----------------------------------------------------------------------

    def _start_task(self, action: str) -> None:
        commands = core.build_commands()
        command = commands.get(action)
        if command is None:
            self._output.append_lines([f"launcher: unknown action '{action}'"])
            return

        try:
            self._runner.start(command, self._repo)
        except RuntimeError as error:
            self._output.append_lines([f"launcher: {error}"])
            return

        self._output.append_lines([f"$ {' '.join(command)}", ""])
        self._taskLabel.setText(action.replace("_", " "))

    def _open_editor(self) -> None:
        command = core.build_commands()["run_editor"]
        project = self._selected_project()

        # The editor takes a project directory and an optional scene, matching its --project/--scene flags; passing them here
        # is what makes the launcher's "Open editor" mean "open this project" rather than "open whatever is in cwd".
        if project is not None:
            command = [*command, "--project", str(project.parent), "--scene", str(project)]

        try:
            self._runner.start(command, self._repo)
        except RuntimeError as error:
            self._output.append_lines([f"launcher: {error}"])
            return

        self._output.append_lines([f"$ {' '.join(command)}", ""])
        self._taskLabel.setText("editor")

    def _selected_project(self) -> Path | None:
        item = self._projectList.currentItem()
        if item is None:
            return None
        raw = item.data(Qt.ItemDataRole.UserRole)
        return Path(str(raw)) if raw else None

    def _open_project_explorer(self) -> None:
        project = self._selected_project()
        target = project.parent if project is not None else self._projectsDirectory

        command = core.project_explorer_command(target)
        if not command:
            self._output.append_lines([f"launcher: no file browser found; open {target} yourself"])
            return

        try:
            # Not through TaskRunner: a file browser is not a build step, and running it as one would hold the editor button
            # hostage until the browser was closed.
            subprocess.Popen(command, cwd=str(self._repo))  # noqa: S603 - argv composed by core, never a shell string
        except OSError as error:
            self._output.append_lines([f"launcher: could not open {target}: {error}"])

    def _stop_task(self) -> None:
        if self._runner.cancel():
            self._output.append_lines(["launcher: requested stop"])
        self._taskLabel.setText("stopping…")

    def _clear_output(self) -> None:
        self._output.clear_output()

    def _drain_output(self) -> None:
        lines = list(self._runner.poll_output())
        if lines:
            self._output.append_lines(lines)

        running = self._runner.is_running
        self._stopButton.setEnabled(running)
        if not running and self._taskLabel.text() != "idle":
            self._taskLabel.setText("idle")
            self.refresh_state()

    # --- window chrome ----------------------------------------------------------------

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        """Draws the glass body: a rounded translucent panel with a soft radial highlight.

        Painted rather than styled: Qt's stylesheet translucency on a top-level widget is unreliable across platforms, and on
        X11 without a compositor any translucency renders as solid black — so the body is drawn here with an alpha that degrades
        to an opaque dark panel rather than disappearing. (Wayland always composites; X11 may not.)
        """
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        rect = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        path = QPainterPath()
        path.addRoundedRect(rect, 20, 20)

        painter.fillPath(path, QColor(PANEL_STYLE))

        highlight = QRadialGradient(rect.width() * 0.28, rect.height() * 0.12, rect.width() * 0.9)
        highlight.setColorAt(0.0, QColor(150, 190, 240, 34))
        highlight.setColorAt(1.0, QColor(0, 0, 0, 0))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(highlight)
        painter.drawPath(path)

        painter.setPen(QPen(QColor(255, 255, 255, 44), 1.0))
        painter.drawPath(path)
        painter.end()

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if event.button() == Qt.MouseButton.LeftButton and event.position().y() < 64:
            # Drag by the header only, so a click on a card never moves the window.
            self._dragOffset = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
            event.accept()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if self._dragOffset is not None and event.buttons() & Qt.MouseButton.LeftButton:
            self.move(event.globalPosition().toPoint() - self._dragOffset)
            event.accept()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self._dragOffset = None
        event.accept()

    def keyPressEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if event.key() == Qt.Key.Key_Escape:
            self.close()
        else:
            super().keyPressEvent(event)


def apply_app_style(application: QApplication) -> None:
    """Font fallbacks and the Fusion base style, set once for the process.

    Inter/JetBrains Mono are named first with system fallbacks: naming a font that may be absent is fine in Qt (it
    substitutes), and refusing to start on a machine without one particular typeface would be an odd thing for a launcher to do.
    Fusion is chosen because it is the same on every platform — the glass styling assumes known metrics, and deferring to the
    native style would let GTK/Windows theme overrides punch opaque rectangles through the translucency.
    """
    application.setStyle("Fusion")

    font = QFont()
    font.setFamilies(["Inter", "Cantarell", "Segoe UI", "DejaVu Sans"])
    font.setPointSize(10)
    application.setFont(font)
