#pragma once

#include <string>
#include <string_view>

#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Serialization/JsonValue.h"

namespace Hollowpoint::Serialization
{

using Core::Size;

// ---------------------------------------------------------------------------
// Where and why a parse failed.
//
// Positions are byte offsets plus a line/column pair, because the two audiences differ: tools want the offset to
// slice the buffer, humans want "line 413, column 22" and will not convert anything. Both are computed during the
// single pass rather than derived afterwards, which keeps them exact even for inputs that fail inside a nested
// escape sequence.
// ---------------------------------------------------------------------------
struct JsonError
{
    Size Offset = 0;      //!< Byte offset of the offending character.
    Size Line = 1;        //!< 1-based line number.
    Size Column = 1;      //!< 1-based column within the line, in bytes.
    std::string Message;  //!< What went wrong, phrased for a human.
    std::string Context;  //!< The offending line, truncated around the fault, with a caret under it.

    //! "path.json (413:22): expected ',' or '}' in object" — ready for a log line.
    [[nodiscard]] std::string Format(const std::string& sourceName) const;
};

// ---------------------------------------------------------------------------
// JsonDocument — a parsed document plus the errors found while parsing it.
//
// Parsing never throws. A malformed scene file is expected input (hand-edited, truncated download, version skew),
// and an engine that aborts a load with an exception has no chance to report which of twenty queued assets broke or
// to fall back to a previous good version. So the entry points below always return: `Success` says whether the tree
// is usable, `Error` explains what did not parse, and `Root` holds whatever was recovered.
//
// Partial results are deliberately **not** offered as if they were valid: on failure callers must treat Root as
// unspecified. Half a scene loaded silently is worse than no scene plus an error message.
// ---------------------------------------------------------------------------
struct JsonDocument
{
    bool Success = false;
    JsonValue Root;
    JsonError Error;

    //! Convenience for callers that only need the value and already checked Success.
    [[nodiscard]] const JsonValue& operator*() const noexcept
    {
        return Root;
    }
};

// Parsing options. Grouped so future limits (string length, allocation budget) do not change signatures.
struct JsonParseOptions
{
    //! Maximum nesting depth. Guards against a hostile or corrupt file exhausting the stack: the parser recurses
    //! once per open bracket, so an unbounded depth turns "[[[[…" in a downloaded asset into a stack overflow.
    Size MaxDepth = 256;
};

//! Parses a complete JSON document. On failure, `Success` is false and `Error` describes the first fault; later
//! faults are not collected, because the first one usually explains the rest and reporting twenty syntax errors for
//! one stray comma trains people to ignore all of them.
JsonDocument ParseJson(std::string_view text, const JsonParseOptions& options = {});

//! Parses and, on success, moves the root out. Returns std::nullopt-style failure via `outError`.
bool TryParseJson(std::string_view text, JsonValue& outRoot, JsonError* outError = nullptr,
                  const JsonParseOptions& options = {});

}  // namespace Hollowpoint::Serialization
