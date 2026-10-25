#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Runtime/RuntimeConfig.h"

// ---------------------------------------------------------------------------
// Loading a RuntimeConfig from JSON.
//
// Why separate from the struct: RuntimeConfig is a value type that tests and command lines fill in directly. Giving it
// a `Load(path)` member would mean every consumer of the config drags in the JSON parser, and would make "parse then
// use" look like one operation when they have very different failure modes — a malformed file is a user-facing error to
// report with a line number, while using a config never fails.
//
// Unknown keys are reported, not fatal. A config written for a newer engine (or one with a typo'd key) must still run:
// the alternative is that an unrecognised word in a settings file stops the engine from starting, which turns a
// harmless forward incompatibility into a support ticket. The report exists so the typo is still visible in a log.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Runtime
{

//! Parsed result: the config plus anything worth telling a human about the file.
struct RuntimeConfigLoadResult
{
    RuntimeConfig Config;

    //! False when the file could not be read or was not valid JSON — in which case `Config` holds only defaults and
    //! must not be used as "the project's settings". A missing optional config is NOT a failure: callers decide whether
    //! it is.
    bool Succeeded = false;

    //! Human-readable problems: parse errors, out-of-range values clamped, unrecognised keys. Empty in the common case.
    std::string Report;
};

//! Parses `jsonText` as a runtime configuration. Never throws; every problem lands in `Report`.
[[nodiscard]] HP_RUNTIME_API RuntimeConfigLoadResult ParseRuntimeConfigJson(std::string_view jsonText);

//! Reads and parses a file. Returns Succeeded=false with the reason when it cannot be read (see Core::FileSystem's
//! error classification — "file not found" and "no permission" are different things to tell a user).
[[nodiscard]] HP_RUNTIME_API RuntimeConfigLoadResult LoadRuntimeConfigFile(const std::string& path);

//! Serialises a config back to JSON text. Used by `--dump-config` so a user has a documented, correct-by-construction
//! example rather than a hand-written one that drifts from the real defaults.
[[nodiscard]] HP_RUNTIME_API std::string SaveRuntimeConfigJson(const RuntimeConfig& config);

}  // namespace Hollowpoint::Runtime
