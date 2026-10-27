#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

// ---------------------------------------------------------------------------
// EditorConsole — the editor's command registry, and the reason every button works without a mouse.
//
// AGENTS.md requires editor actions to be scriptable/console-driven. Implemented as a registry the UI calls rather
// than a text parser the UI also happens to offer: each panel action (select entity, load scene, create cube, save) is
// registered once as a name + handler and both the widget and the console line go through the same call. Two
// implementations of one action is how "the button saves, the command doesn't" bugs happen.
//
// A text box still exists (it is what makes an editor feel like UE's), but it is a *front end* to this registry: it
// splits a line into a name and arguments and invokes the same handler. That ordering matters for testing — every
// command can be driven programmatically with no UI and no window, which is how the suites below assert on behaviour
// rather than on pixels.
//
// Commands return their result as text (the console's reply) or an error string. Not a status enum: the human-facing
// message *is* the useful output for a tool command, and forcing callers to re-derive "what did this do" from a code
// would put that text in the caller instead of here, duplicated per call site.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Index;
using Core::Size;

//! One registered command.
struct ConsoleCommand
{
    std::string Name;     //!< Lower-case invocation name, e.g. "scene.save".
    std::string Usage;    //!< One line shown by `help` and on a wrong argument count.
    std::string Summary;  //!< Short description for `help` listings.

    //! Executes the command. `arguments` are the whitespace-separated tokens after the name (already split, empty when
    //  the line had none). Returns the console reply, or sets `outError` and returns an empty string on failure.
    std::function<bool(const std::vector<std::string>& arguments, std::string& outReply, std::string& outError)>
        Execute;
};

//! Commands whose argument count is fixed — the common case, checked once here rather than in every handler.
enum class ArgumentCount : Core::UInt8
{
    Exact,    //!< Exactly `Expected` arguments.
    AtLeast,  //!< At least `Expected`.
    Any,      //!< Handler decides.
};

//! The command registry. One instance per editor; commands are registered at startup by whichever module owns the
//! action, which keeps "what can the console do" answerable by grepping for Register calls.
class HP_EDITOR_API EditorConsole
{
public:
    //! Registers a command. Re-registering a name replaces it and returns false — a silently ignored duplicate would
    //  leave whichever registration happened to win first, i.e. whichever translation unit linked first, in charge.
    bool Register(ConsoleCommand command);

    //! Convenience wrapper for the fixed-arity case: validates the argument count against `policy`/`expected` before
    //  calling `handler`, so handlers do not each re-implement "wrong number of arguments" (and disagree on the
    //  wording). The usage line is generated from the command name plus one placeholder per expected argument.
    bool RegisterFixed(
        std::string name, std::string summary, ArgumentCount policy, Index expected,
        std::function<bool(const std::vector<std::string>& arguments, std::string& outReply, std::string& outError)>
            handler);

    //! Invokes a command by name. Returns false with `outError` set when the name is unknown or the handler failed.
    bool Execute(const std::string& name, const std::vector<std::string>& arguments, std::string& outReply,
                 std::string& outError);

    //! Splits one console line (name + whitespace-separated arguments, `"quoted groups"` supported) and executes it.
    //  Returns false on an unknown command or a handler failure; `outError` carries the message either way. An empty
    //  line is not an error — pressing Enter in an empty box should produce nothing rather than "unknown command".
    bool ExecuteLine(const std::string& line, std::string& outReply, std::string& outError);

    //! Splits a line into tokens. Public because the argument-splitting rules (quotes, escapes) are exactly the part
    //  worth testing on their own: a stray quote swallowing the rest of a line is a bug users hit within minutes.
    [[nodiscard]] static std::vector<std::string> SplitArguments(std::string_view line);

    [[nodiscard]] bool HasCommand(const std::string& name) const;

    //! Names beginning with `prefix`, sorted — feeds the console's completion list. An empty prefix lists everything,
    //  in sorted order, which is what `help` prints.
    [[nodiscard]] std::vector<std::string> Complete(std::string_view prefix) const;

    [[nodiscard]] const ConsoleCommand* Find(const std::string& name) const;

    [[nodiscard]] Size GetCommandCount() const noexcept;

private:
    // Insertion-ordered storage, not a map: `help` output should read in registration order (engine commands first,
    // then whatever a project added), and the command count is small enough that lookup is a linear scan. A tool with
    // hundreds of commands would add an index, not change this contract.
    std::vector<ConsoleCommand> m_Commands;
};

//! Registers the editor's built-in commands against `console` (help, plus every action whose owner is passed in).
//  Idempotent — re-registering replaces rather than duplicating, so a test can call it on a fresh registry.
void RegisterEditorCommands(EditorConsole& console);

}  // namespace Hollowpoint::Editor
