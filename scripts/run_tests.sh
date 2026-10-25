#!/usr/bin/env bash
# Full quality gate for Hollowpoint: format check -> configure -> build -> tests -> sanitizers.
#
# Usage:
#   scripts/run_tests.sh                 # configure+build+test the default (debug) preset
#   scripts/run_tests.sh --preset linux-relwithdebinfo
#   scripts/run_tests.sh --asan          # additionally run the whole suite under ASan/UBSan
#   scripts/run_tests.sh --quick         # skip the sanitizer pass and the format check
#
# Exit code is non-zero if any stage fails. This is the command to run before every commit.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PRESET="linux-debug"
RUN_ASAN=0
QUICK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2 ;;
        --asan) RUN_ASAN=1; shift ;;
        --quick) QUICK=1; shift ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mFAILED: %s\033[0m\n' "$*" >&2; exit 1; }

# Prefer a cmake/ninja from Homebrew or PATH as configured by the developer.
command -v cmake >/dev/null || fail "cmake not found on PATH"

if [[ "$QUICK" == "0" ]]; then
    step "Format check (clang-format)"
    if command -v clang-format >/dev/null; then
        python3 scripts/format.py --check || fail "code style"
    else
        echo "clang-format not found; skipping (install it to enable this gate)"
    fi

    # Docs drift the same way code does, and the status table is what a newcomer uses to judge risk: four of its
    # "N cases" numbers had quietly stopped matching the sources. Cheap enough to always check, and it needs no build.
    step "Documented test counts (docs/ARCHITECTURE.md)"
    python3 scripts/check_doc_counts.py || fail "documented case counts out of date"
fi

step "Configure (${PRESET})"
cmake --preset "$PRESET" >/dev/null || fail "configure ${PRESET}"

BUILD_DIR="$(python3 - "$PRESET" <<'PY'
import json, sys, pathlib
preset = sys.argv[1]
data = json.loads(pathlib.Path("CMakePresets.json").read_text())
binary = None
for entry in data["configurePresets"]:
    if entry.get("name") == preset:
        binary = entry.get("binaryDir")
print(pathlib.Path(binary.replace("${sourceDir}", ".")).resolve() if binary else "build")
PY
)"

step "Build (${PRESET})"
cmake --build --preset "$PRESET" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
    || fail "build ${PRESET}"

step "Tests (${PRESET})"
ctest --test-dir "$BUILD_DIR" --output-on-failure || fail "tests ${PRESET}"

if [[ "$RUN_ASAN" == "1" ]]; then
    step "Configure (linux-asan)"
    cmake --preset linux-asan >/dev/null || fail "configure linux-asan"

    step "Build (linux-asan)"
    cmake --build --preset linux-asan -j "$(nproc)" >/dev/null || fail "build linux-asan"

    step "Tests under AddressSanitizer + UndefinedBehaviorSanitizer"
    ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:color=always" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0" \
        ctest --test-dir build/asan --output-on-failure || fail "sanitizer tests"
fi

printf '\n\033[1;32mAll checks passed.\033[0m\n'
