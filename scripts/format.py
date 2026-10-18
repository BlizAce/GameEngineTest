#!/usr/bin/env python3
"""Format check / apply for the whole repository using clang-format.

Usage:
    scripts/format.py            # format files in place
    scripts/format.py --check    # exit non-zero if anything is unformatted (CI gate)
    scripts/format.py <path>...  # restrict to specific files

Exit codes: 0 = clean (or formatted successfully), 1 = unformatted files (--check), 2 = tool error.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".cxx", ".c"}
EXCLUDED_DIRECTORY_NAMES = {"build", "out", "third_party", ".git", "__pycache__", "node_modules"}


def find_sources(root: Path) -> list[Path]:
    sources: list[Path] = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        if any(part in EXCLUDED_DIRECTORY_NAMES for part in path.relative_to(root).parts):
            continue
        sources.append(path)
    return sources


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="only report, do not write")
    parser.add_argument("paths", nargs="*", help="files to process (default: whole repository)")
    arguments = parser.parse_args(argv)

    repositoryRoot = Path(__file__).resolve().parent.parent
    clangFormat = shutil.which("clang-format")
    if clangFormat is None:
        print("error: clang-format not found on PATH", file=sys.stderr)
        return 2

    sources = [Path(p) for p in arguments.paths] if arguments.paths else find_sources(repositoryRoot)
    if not sources:
        print("no source files to check")
        return 0

    unformatted: list[Path] = []
    for source in sources:
        result = subprocess.run([clangFormat, str(source)], capture_output=True, text=True, check=False)
        if result.returncode != 0:
            print(f"error: clang-format failed on {source}:\n{result.stderr}", file=sys.stderr)
            return 2

        original = source.read_text(encoding="utf-8")
        formatted = result.stdout
        if formatted == original:
            continue

        if arguments.check:
            unformatted.append(source)
        else:
            source.write_text(formatted, encoding="utf-8")
            print(f"formatted {source.relative_to(repositoryRoot)}")

    if arguments.check and unformatted:
        print(f"{len(unformatted)} file(s) need formatting:", file=sys.stderr)
        for source in unformatted:
            print(f"  {source.relative_to(repositoryRoot)}", file=sys.stderr)
        print("run 'scripts/format.py' to fix", file=sys.stderr)
        return 1

    if not arguments.check:
        print(f"checked {len(sources)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
