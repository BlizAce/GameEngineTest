#!/usr/bin/env bash
# Hollowpoint launcher — a graphical front door for the repository.
#
# Usage:
#   ./run.sh              # open the launcher
#   ./run.sh --help       # this text
#   ./run.sh --check      # verify the toolchain and report, without opening a window
#
# Why a shell script rather than a Python entry point: it can check for its own prerequisites (python3, PySide6) and say
# what to install in one line, which a crash traceback does not. Everything else — the actual UI, the commands it runs —
# lives in tools/launcher/, because a build step embedded in shell quoting is untestable and this one is not simple.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER="$REPO_ROOT/tools/launcher/main.py"

if [[ ! -f "$LAUNCHER" ]]; then
    echo "launcher: $LAUNCHER is missing — the checkout looks incomplete" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "launcher: python3 is required (it drives the UI and every build step)" >&2
    echo "          install it with your package manager, e.g. sudo dnf install python3" >&2
    exit 1
fi

if [[ "${1:-}" == "--check" ]]; then
    # Same checks the launcher shows on startup, without needing a display — this is what CI and ssh sessions use.
    exec python3 "$LAUNCHER" --check "$@"
fi

if ! python3 -c "import PySide6" >/dev/null 2>&1; then
    echo "launcher: PySide6 (Qt 6 for Python) is not installed." >&2
    echo "" >&2
    echo "  python3 -m pip install --user 'PySide6>=6.6'" >&2
    echo "" >&2
    echo "The launcher is optional convenience; the engine itself needs nothing but CMake, Ninja and a" >&2
    echo "C++20 compiler, and builds directly:" >&2
    echo "  cmake --preset linux-debug && cmake --build build/debug -j && ctest --test-dir build/debug" >&2
    exit 1
fi

exec python3 "$LAUNCHER" "$@"
