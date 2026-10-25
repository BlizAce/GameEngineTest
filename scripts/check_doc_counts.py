#!/usr/bin/env python3
"""Check that every test-case count in docs/ARCHITECTURE.md matches the sources.

Why this exists: the status table lists "N cases" per module, and four of those numbers had drifted from reality — two
understating suites that had grown, and one pointing at a directory while describing only transforms (so tests/Scene read as
22 cases when it holds 85). A doc that quietly goes stale is worse than none, because that table is exactly what a newcomer
trusts to decide where the risk is.

Counts are never maintained by hand: run this after adding or removing a TEST_CASE and paste what it says. It reads
`TEST_CASE` occurrences from sources rather than asking the test binaries, so it works without a build — and reports what
exists rather than what happened to run on this host, which is fewer wherever GPU or display cases skip.

Usage: check_doc_counts.py [--quiet]      exit 0 = every documented claim correct
"""

import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(REPO, "docs", "ARCHITECTURE.md")

# `tests/Module/File.cpp` — 43 cases    /    `tests/Module/` — 90 cases
# The optional parenthetical is a breakdown ("109 cases (71 JSON, 38 bridge)") and is not checked separately.
CLAIM = re.compile(r"`tests/([A-Za-z]+)/?(?:([A-Za-z0-9_]+\.cpp))?`?\s*(\([^)]*\))?\s*[—-]\s*(\d+)\s+cases")


def count_cases(path: str) -> int:
    with open(path, encoding="utf-8") as handle:
        return handle.read().count("TEST_CASE")


def main() -> int:
    quiet = "--quiet" in sys.argv

    with open(DOC, encoding="utf-8") as handle:
        lines = handle.read().split("\n")

    problems = []
    checked = 0
    # Which files a row claims, per module, plus which modules have a directory-wide row. Both are needed to tell "these rows
    # describe one file each and the rest of the module is undocumented" (a gap worth failing) from "the directory is covered
    # and these rows add detail" (fine).
    scoped: dict[str, set[str]] = {}
    wildcard_modules: set[str] = set()

    for number, line in enumerate(lines, 1):
        match = CLAIM.search(line)
        if not match:
            continue
        checked += 1
        module, filename, _paren, claimed = match.groups()
        claimed_count = int(claimed)

        if filename:
            path = os.path.join(REPO, "tests", module, filename)
            if not os.path.exists(path):
                problems.append(f"L{number}: {module}/{filename} does not exist")
                continue
            actual = count_cases(path)
            scoped.setdefault(module, set()).add(filename)
        else:
            files = sorted(glob.glob(os.path.join(REPO, "tests", module, "*.cpp")))
            if not files:
                problems.append(f"L{number}: no test sources under tests/{module}/")
                continue
            actual = sum(count_cases(f) for f in files)
            wildcard_modules.add(module)

        if actual != claimed_count:
            label = f"{module}/{filename}" if filename else f"{module}/*"
            problems.append(f"L{number}: {label} doc says {claimed_count}, sources have {actual}")

    for module, named in scoped.items():
        if module in wildcard_modules:
            continue
        files = sorted(glob.glob(os.path.join(REPO, "tests", module, "*.cpp")))
        missing = [os.path.basename(f) for f in files if os.path.basename(f) not in named]
        if missing:
            total = sum(count_cases(f) for f in files)
            covered = sum(count_cases(os.path.join(REPO, "tests", module, f)) for f in named)
            problems.append(
                f"{module}: rows name {covered} of the directory's {total} cases; no row covers {', '.join(missing)}"
            )

    if not quiet:
        print(f"checked {checked} documented case counts")
    for problem in problems:
        print(f"MISMATCH {problem}")
    if problems:
        print("\nUpdate docs/ARCHITECTURE.md to the measured numbers (this script is the source of truth).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
