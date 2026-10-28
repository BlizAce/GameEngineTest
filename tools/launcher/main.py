"""Hollowpoint launcher entry point.

Deliberately tiny, and deliberately Qt-free until after argument handling: `--check` has to work over ssh and on a machine
where PySide6 is absent, which is the same reason the engine keeps its logic out of main(). Everything that can be wrong —
which command an action runs, whether the tree is configured, what a missing tool means — lives in `core.py` and is tested
there without a display.

Usage:
  python3 tools/launcher/main.py             open the launcher window
  python3 tools/launcher/main.py --check     print toolchain/build status and exit (no Qt, no window)
"""

from __future__ import annotations

import sys
from pathlib import Path

import core

# The repository root is two levels up from this file (tools/launcher/), computed rather than assumed from the working
# directory so the launcher works when started from a desktop entry or a symlink.
REPO_ROOT = Path(__file__).resolve().parents[2]


def check() -> int:
    """Headless status report — the same facts the window shows, for CI and remote shells.

    Returns non-zero only for a genuinely missing prerequisite, so it can gate a script; an unconfigured build tree is a
    normal state to be in, not an error.
    """
    print(f"hollowpoint launcher — {REPO_ROOT}")

    toolchain = core.check_toolchain()
    for requirement, present in toolchain:
        mark = "ok  " if present else "MISS"
        line = f"  [{mark}] {requirement.name:16s} {requirement.purpose}"
        if not present:
            line += f"\n         install: {requirement.install_hint()}"
        print(line)

    depsMark = "ok  " if core.dependencies_installed() else "MISS"
    print(f"  [{depsMark}] {'dependencies':16s} {core.DEPENDENCY_CACHE}")

    for preset in core.PRESETS:
        state = "configured" if core.is_configured(REPO_ROOT, preset) else "not configured"
        if core.is_built(REPO_ROOT, preset):
            state += ", built"
        print(f"  [    ] {preset:16s} {state}")

    projects = core.discover_projects(REPO_ROOT / "projects")
    print(f"  [    ] {'projects':16s} {len(projects)} found in {REPO_ROOT / 'projects'}")
    if projects:
        for path in projects[:10]:
            print(f"           - {path.name}")

    return 0 if all(present for _, present in toolchain) else 1


def gui() -> int:
    from PySide6.QtWidgets import QApplication

    import ui

    application = QApplication(sys.argv)
    ui.apply_app_style(application)

    window = ui.LauncherWindow(REPO_ROOT)
    window.refresh_projects()
    window.show()
    return application.exec()


def main(argv: list[str]) -> int:
    if "--check" in argv:
        return check()
    try:
        return gui()
    except ImportError as error:
        # Reached only when run.sh's own PySide6 probe was bypassed (a renamed interpreter, a venv switch). Saying what to
        # install beats an ImportError traceback, which tells a user nothing actionable.
        print(f"launcher: the graphical launcher needs PySide6 ({error}).", file=sys.stderr)
        print("  python3 -m pip install --user 'PySide6>=6.6'", file=sys.stderr)
        print("Or run --check for a headless status report.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
