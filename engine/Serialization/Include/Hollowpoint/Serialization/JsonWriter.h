#pragma once

#include <string>
#include <string_view>

#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Serialization/JsonValue.h"

namespace Hollowpoint::Serialization
{

using Core::Size;

// How to format a document on output.
struct JsonWriteOptions
{
    //! Indent nested levels with this many spaces; 0 selects the compact single-line form. Compact is for network
    //! and logs, indented is what goes into version control because it diffs per field instead of per file.
    Size IndentSpaces = 2;

    //! Append a trailing newline. Text files should end with one (POSIX tools, git's "no newline at end of file"
    //! warning), so this is on by default for the indented form.
    bool TrailingNewline = true;

    //! Escape every non-ASCII code point as \\uXXXX. Off by default: scene files are UTF-8 and readable beats
    //! portable-but-opaque when both are valid. Turn it on for consumers that mangle encoding in transit.
    bool EscapeNonAscii = false;

    //! Sort object members alphabetically instead of preserving insertion order. Only useful for golden-file tests
    //! where two independently built documents must compare byte-for-byte; production saves must not, because it
    //! destroys the field order that makes a scene diff readable.
    bool SortObjectKeys = false;
};

//! Writes a document as JSON text. Output is deterministic: the same tree always produces the same bytes on every
//! platform, which is what makes committed scene files reviewable and asset hashes meaningful.
std::string WriteJson(const JsonValue& value, const JsonWriteOptions& options = {});

//! Single-line form with no insignificant whitespace. Intended for logs and network framing.
std::string WriteCompactJson(const JsonValue& value);

// ---------------------------------------------------------------------------
// Number formatting.
//
// Reals use std::to_chars' shortest representation that round-trips exactly, so 0.1 writes as "0.1" and not
// "0.10000000000000001". That is a correctness property, not cosmetics: the alternative (fixed precision) either
// loses bits or pads every number in a scene file, and both show up as churn in version control and as values that
// change on every load/save cycle. Integers never pass through a float, so entity indices and 64-bit hashes survive
// byte-identical.
// ---------------------------------------------------------------------------

//! Formats a double as the shortest text that parses back to exactly the same value.
std::string FormatDouble(double value);

}  // namespace Hollowpoint::Serialization
