// Hollowpoint editor executable.
//
// Argument parsing and exit codes only — every behaviour worth testing lives in HollowpointEditor and is tested there
// (tests/Editor). This mirrors engine/Runtime/Src/Main.cpp deliberately: an executable whose main() contains logic is
// an untestable main(), and the two entry points sharing a shape is what keeps "how do I run this?" to one answer.
//
// Exit codes (the consumer is automation, so they are part of the interface):
//   0  ran normally — window closed by the user, or the frame limit reached; both are successes
//   1  startup failed — no display, no Vulkan device, project directory unusable
//   2  bad command line — nothing was attempted, and the usage text says why
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/EditorApplication.h"

namespace
{

using namespace Hollowpoint;
using Editor::EditorApplication;
using Editor::EditorConfig;
using Editor::EditorStopReason;

void PrintUsage(const char* programName)
{
    std::printf(
        "Hollowpoint editor\n"
        "\n"
        "usage: %s [options]\n"
        "\n"
        "  --project <dir>      project root (content browser + relative scene paths)\n"
        "                       default: the working directory\n"
        "  --scene <file>       .hpscene to open at startup\n"
        "  --width <n>          window width in content pixels (default 1600)\n"
        "  --height <n>         window height in content pixels (default 900)\n"
        "  --frames <n>         exit after n composed frames (automated smoke test)\n"
        "  --screenshot <path>  write a PNG of the last frame before exiting\n"
        "  --command <line>     run one console command after startup; repeatable\n"
        "  --play               tick gameplay scripts (default is edit mode: paused)\n"
        "  --hidden             start unmapped (capture runs, no screen stealing)\n"
        "  --no-validation      disable Vulkan validation layers\n"
        "  --verbose            log at debug level\n"
        "  --log-file <path>    also append logs to a file\n"
        "  --help               this text\n"
        "\n"
        "Every action the panels offer is also a console command — `help` inside the editor, or\n"
        "  %s --frames 3 --command \"entity.create Cube engine:cube\" --command \"scene.save out.hpscene\"\n"
        "to drive it from a script with no window interaction at all.\n",
        programName, programName);
}

[[nodiscard]] bool ReadValue(int argc, char** argv, int& index, std::string& outValue, std::string& outError)
{
    if (index + 1 >= argc)
    {
        outError = "option '" + std::string(argv[index]) + "' needs a value";
        return false;
    }
    outValue = argv[++index];
    return true;
}

[[nodiscard]] bool ParseUnsigned(const std::string& text, unsigned long long& outValue)
{
    if (text.empty())
    {
        return false;
    }
    try
    {
        size_t consumed = 0;
        outValue = std::stoull(text, &consumed);
        // Trailing garbage is a typo, not a number with an ignored suffix — same rule the editor's own argument parsing
        // uses, so `--width 800px` fails here exactly as `entity.set ... 0.2x` would.
        return consumed == text.size();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const char* programName = argc > 0 ? argv[0] : "hollowpoint-editor";

    EditorConfig config;
    bool verbose = false;
    std::string logFilePath;
    std::vector<std::string> commandsToRun;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        std::string value;
        std::string error;

        if (argument == "--help" || argument == "-h")
        {
            PrintUsage(programName);
            return 0;
        }
        if (argument == "--verbose")
        {
            verbose = true;
            continue;
        }
        if (argument == "--hidden")
        {
            config.StartVisible = false;
            continue;
        }
        if (argument == "--no-validation")
        {
            config.EnableValidation = false;
            continue;
        }
        if (argument == "--play")
        {
            config.PlayModeScripts = true;
            continue;
        }

        auto readInto = [&](std::string& destination) -> bool {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return false;
            }
            destination = value;
            return true;
        };

        if (argument == "--project")
        {
            if (!readInto(config.ProjectDirectory))
            {
                return 2;
            }
            continue;
        }
        if (argument == "--scene")
        {
            if (!readInto(config.ScenePath))
            {
                return 2;
            }
            continue;
        }
        if (argument == "--screenshot")
        {
            if (!readInto(config.ScreenshotPath))
            {
                return 2;
            }
            continue;
        }
        if (argument == "--log-file")
        {
            if (!readInto(logFilePath))
            {
                return 2;
            }
            continue;
        }
        if (argument == "--command")
        {
            std::string command;
            if (!readInto(command))
            {
                return 2;
            }
            commandsToRun.push_back(std::move(command));
            continue;
        }
        if (argument == "--width" || argument == "--height" || argument == "--frames")
        {
            if (!readInto(value))
            {
                return 2;
            }

            unsigned long long parsed = 0;
            if (!ParseUnsigned(value, parsed))
            {
                std::fprintf(stderr, "%s: '%s' is not a valid value for %.*s\n", programName, value.c_str(),
                             static_cast<int>(argument.size()), argument.data());
                return 2;
            }

            if (argument == "--width")
            {
                config.Width = static_cast<Core::UInt32>(parsed);
            }
            else if (argument == "--height")
            {
                config.Height = static_cast<Core::UInt32>(parsed);
            }
            else
            {
                config.MaxFrameCount = parsed;
            }
            continue;
        }

        std::fprintf(stderr, "%s: unknown option '%.*s' (try --help)\n", programName, static_cast<int>(argument.size()),
                     argument.data());
        return 2;
    }

    // Attach sinks before anything can log: the dispatcher has no sinks by default, so without this every
    // "returns false with the reason logged" contract in the engine produces no output and a failed startup exits 1
    // saying nothing (the same reasoning engine/Runtime/Src/Main.cpp gives).
    const Core::LogLevel level = verbose ? Core::LogLevel::Debug : Core::LogLevel::Info;
    Core::AttachStandardLogSink(level);
    if (!logFilePath.empty() && Core::AttachFileLogSink(logFilePath, level) == nullptr)
    {
        std::fprintf(stderr, "%s: could not open the log file '%s'\n", programName, logFilePath.c_str());
    }

    HP_LOG_INFO("EDITOR", "starting editor (project \"{}\")",
                config.ProjectDirectory.empty() ? "<working directory>" : config.ProjectDirectory);


    EditorApplication editor(std::move(config));
    if (!editor.Start())
    {
        std::fprintf(stderr, "%s: editor startup failed (see log)\n", programName);
        return 1;
    }

    // Commands run before the loop starts, so `--frames 1 --command "entity.create ..."` composes exactly one frame
    // containing their effect. Running them after the first frame instead would make automated captures depend on how
    // many frames the compositor happened to deliver during startup.
    for (const std::string& command : commandsToRun)
    {
        editor.RunStartupCommand(command);
    }

    const EditorStopReason reason = editor.Run();

    HP_LOG_INFO("EDITOR", "stopped: {}", Editor::EditorStopReasonName(reason));
    std::printf("editor %s\n", Editor::EditorStopReasonName(reason));

    return reason == EditorStopReason::StartupFailed ? 1 : 0;
}
