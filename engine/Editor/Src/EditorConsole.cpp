#include <algorithm>
#include <cctype>
#include <sstream>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/EditorConsole.h"

// ---------------------------------------------------------------------------
// See the header. Two pieces of care worth naming:
//   * argument splitting handles quoted groups, because scene paths with spaces are normal on every OS and a splitter
//     that ignored quotes would make `scene.load "My Scene.hpscene"` fail in the way users report as "it works without
//     spaces";
//   * unknown command names list close matches. A tool with 40 commands and no suggestions is a tool where people stop
//     using the console, and the fallback (a GUI-only path) is exactly what AGENTS.md forbids.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Size;

namespace
{

[[nodiscard]] std::string ToLower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

bool EditorConsole::Register(ConsoleCommand command)
{
    if (command.Name.empty() || !command.Execute)
    {
        HP_LOG_ERROR("EDITOR", "refusing to register a console command with no name or handler");
        return false;
    }

    const std::string name = ToLower(command.Name);

    for (ConsoleCommand& existing : m_Commands)
    {
        if (existing.Name == name)
        {
            // Replace, and report it. A duplicate registration is a bug in the registering code (two modules chose the
            // same name), but aborting the editor over it is worse than keeping the later one — which is deterministic
            // given registration order, unlike "first wins" across translation units.
            HP_LOG_WARNING("EDITOR", "console command \"{}\" re-registered; the later definition wins", name);
            existing = std::move(command);
            existing.Name = name;
            return false;
        }
    }

    command.Name = name;
    m_Commands.push_back(std::move(command));
    return true;
}

bool EditorConsole::RegisterFixed(
    std::string name, std::string summary, ArgumentCount policy, Index expected,
    std::function<bool(const std::vector<std::string>&, std::string&, std::string&)> handler)
{
    // The usage line is generated rather than hand-written per command: an out-of-date usage string is worse than none,
    // because it is what a user trusts when the argument check rejects their line.
    std::ostringstream usage;
    usage << ToLower(name);

    const Index minimum = expected > 0 ? expected : 0;
    for (Index index = 0; index < minimum; ++index)
    {
        usage << " <arg" << (index + 1) << ">";
    }
    if (policy == ArgumentCount::AtLeast)
    {
        usage << " ...";
    }

    ConsoleCommand command;
    command.Name = std::move(name);
    command.Usage = usage.str();
    command.Summary = std::move(summary);
    command.Execute = [handler = std::move(handler), policy, minimum](const std::vector<std::string>& arguments,
                                                                      std::string& outReply,
                                                                      std::string& outError) -> bool {
        const Size count = arguments.size();
        if (policy == ArgumentCount::Exact && static_cast<Index>(count) != minimum)
        {
            outError = "expected " + std::to_string(minimum) + " argument(s), got " + std::to_string(count);
            return false;
        }
        if (policy == ArgumentCount::AtLeast && static_cast<Index>(count) < minimum)
        {
            outError = "expected at least " + std::to_string(minimum) + " argument(s), got " + std::to_string(count);
            return false;
        }
        return handler(arguments, outReply, outError);
    };

    return Register(std::move(command));
}

bool EditorConsole::Execute(const std::string& name, const std::vector<std::string>& arguments, std::string& outReply,
                            std::string& outError)
{
    const ConsoleCommand* command = Find(name);
    if (command == nullptr)
    {
        outError = "unknown command: " + name;

        // Suggestions, computed only on failure so the hot path stays a scan. Levenshtein would be nicer; a prefix and
        // substring match catches the realistic mistakes (typo at the end, forgetting a namespace prefix) without
        // pulling in an edit-distance table for 40 short strings.
        const std::string lowered = ToLower(name);
        std::vector<std::string> close;
        for (const ConsoleCommand& candidate : m_Commands)
        {
            if (candidate.Name.rfind(lowered, 0) == 0 || candidate.Name.find(lowered) != std::string::npos)
            {
                close.push_back(candidate.Name);
            }
        }
        if (!close.empty())
        {
            std::sort(close.begin(), close.end());
            outError += " (did you mean ";
            for (Size index = 0; index < close.size() && index < 4u; ++index)
            {
                outError += (index == 0u ? "" : ", ") + close[index];
            }
            outError += "?)";
        }
        return false;
    }

    outReply.clear();
    if (!command->Execute(arguments, outReply, outError))
    {
        if (outError.empty())
        {
            // A handler that fails without explaining itself is a bug in the handler, and saying so here beats an empty
            // console line — which reads as "the command did nothing", indistinguishable from a command that works.
            outError = "command \"" + command->Name + "\" failed without a message";
        }
        return false;
    }
    return true;
}

std::vector<std::string> EditorConsole::SplitArguments(std::string_view line)
{
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;
    bool haveToken = false;  // distinguishes `""` (an empty argument) from nothing at all

    for (Size index = 0; index < line.size(); ++index)
    {
        const char c = line[index];

        if (c == '"')
        {
            // A quote toggles grouping and never appears in the token. `a"b c"` therefore yields `ab c`, matching every
            // shell users will compare against.
            inQuotes = !inQuotes;
            haveToken = true;
            continue;
        }

        if (c == '\\' && inQuotes && index + 1 < line.size() && (line[index + 1] == '"' || line[index + 1] == '\\'))
        {
            // Only \" and \\ are escapes inside quotes. Everything else keeps its backslash, because a Windows path
            // ("C:\\Projects\\scene.hpscene") must survive intact — unescaping \P to P would corrupt exactly the paths
            // quoting exists to protect.
            current += line[index + 1];
            ++index;
            continue;
        }

        if (!inQuotes && (c == ' ' || c == '\t'))
        {
            if (haveToken)
            {
                tokens.push_back(current);
                current.clear();
                haveToken = false;
            }
            continue;
        }

        current += c;
        haveToken = true;
    }

    if (inQuotes)
    {
        // An unterminated quote keeps whatever was typed rather than dropping it: the caller's argument check will
        // report a count mismatch, which names the real problem better than silently passing one fewer argument.
        HP_LOG_WARNING("EDITOR", "unterminated quote in console line; the rest of the line was taken literally");
    }

    if (haveToken)
    {
        tokens.push_back(current);
    }

    return tokens;
}

bool EditorConsole::ExecuteLine(const std::string& line, std::string& outReply, std::string& outError)
{
    const std::vector<std::string> tokens = SplitArguments(line);
    if (tokens.empty())
    {
        outReply.clear();
        outError.clear();
        return true;  // empty input is not an error; see the header
    }

    const std::vector<std::string> arguments(tokens.begin() + 1, tokens.end());
    return Execute(tokens[0], arguments, outReply, outError);
}

bool EditorConsole::HasCommand(const std::string& name) const
{
    return Find(name) != nullptr;
}

const ConsoleCommand* EditorConsole::Find(const std::string& name) const
{
    const std::string lowered = ToLower(name);
    for (const ConsoleCommand& command : m_Commands)
    {
        if (command.Name == lowered)
        {
            return &command;
        }
    }
    return nullptr;
}

std::vector<std::string> EditorConsole::Complete(std::string_view prefix) const
{
    const std::string lowered = ToLower(prefix);

    std::vector<std::string> matches;
    for (const ConsoleCommand& command : m_Commands)
    {
        if (lowered.empty() || command.Name.rfind(lowered, 0) == 0)
        {
            matches.push_back(command.Name);
        }
    }

    // Sorted here rather than kept sorted in storage: registration order is `help`'s order (engine first), and the two
    // orders serve different readers. The list is tens of entries, so sorting on every keystroke is not a cost.
    std::sort(matches.begin(), matches.end());
    return matches;
}

Size EditorConsole::GetCommandCount() const noexcept
{
    return m_Commands.size();
}

}  // namespace Hollowpoint::Editor
