// Hollowpoint runtime executable.
//
// The engine's first runnable artifact: parse a configuration, start the application shell, run frames until something
// says stop. Everything substantive lives in Runtime::RuntimeApplication — this file is argument handling and exit
// codes, kept thin so an embedding host (the editor) can call the same startup path without going through a command
// line.
//
// Exit codes are part of the interface, because the primary consumer is automation:
//   0  ran normally (window closed by the user, or the frame limit reached — both are successes)
//   1  startup failed (no display, no Vulkan device, config unreadable)
//   2  bad command line (nothing was attempted; the usage text says why)
// Distinguishing 1 from 2 matters in a script: "the test machine has no GPU" and "I typo'd --frames" need different
// responses.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Runtime/RuntimeApplication.h"
#include "Hollowpoint/Runtime/RuntimeConfigLoader.h"

namespace
{

using namespace Hollowpoint;
using Runtime::RuntimeApplication;
using Runtime::RuntimeConfig;
using Runtime::RuntimeConfigLoadResult;
using Runtime::StopReason;

void PrintUsage(const char* programName)
{
    std::printf(
        "Hollowpoint runtime\n"
        "\n"
        "usage: %s [options]\n"
        "\n"
        "  --config <file>      JSON configuration file (see --dump-config for the full key list)\n"
        "  --scene <file>       .hpscene to load at startup\n"
        "  --script-dir <path>  directory holding init.lua and --script files (default: scripts)\n"
        "  --script <file>      Lua file to run after the scene; repeatable, run in order\n"
        "  --width <n>          window width in content pixels (default 1280)\n"
        "  --height <n>         window height in content pixels (default 720)\n"
        "  --frames <n>         exit after n frames (default: run until the window closes)\n"
        "  --screenshot <path>  write a PNG of the last frame before exiting\n"
        "  --exposure <n>       exposure applied before the display transform (default 1.0)\n"
        "  --present <mode>     vsync | mailbox | immediate (default vsync)\n"
        "  --no-validation      disable Vulkan validation layers\n"
        "  --no-scripting       start without a Lua VM\n"
        "  --hidden             create the window unmapped (for capture runs)\n"
        "  --verbose            log at debug level\n"
        "  --log-file <path>    also append logs to a file (for launches with no console)\n"
        "  --dump-config        print the effective configuration as JSON and exit\n"
        "  --help               this text\n"
        "\n"
        "example, rendering the shipped demo and saving what it drew:\n"
        "  %s --scene assets/scenes/demo.hpscene --script-dir assets/demos/scripts \\\n"
        "     --frames 60 --screenshot /tmp/hollowpoint.png\n",
        programName, programName);
}

//! True when `argument` equals `name`, having checked the length first — comparing a prefix would accept "--fr" for
//! "--frames", which turns a typo into a silently ignored option.
[[nodiscard]] bool IsOption(std::string_view argument, std::string_view name)
{
    return argument == name;
}

//! Reads the value that follows `argument`, or reports the missing operand. A flag at the end of argv is the common
//! mistake
//! (`--frames` with no number), and "expected a value" beats "unknown option".
bool ReadValue(int argc, char** argv, int& index, std::string& outValue, std::string& outError)
{
    if (index + 1 >= argc)
    {
        outError = std::string(argv[index]) + " expects a value";
        return false;
    }
    ++index;
    outValue = argv[index];
    return true;
}

bool ReadPositiveUInt(const std::string& text, unsigned long long& outValue, std::string& outError)
{
    // strtoull rather than std::stoull: the latter throws on garbage, and catching an exception per argument to print a
    // usage line is heavier than checking endptr. A trailing suffix ("12px") is rejected here rather than silently
    // parsed as 12 — accepting it would make "--width 1080p" produce a 1080-wide window, which is exactly the kind of
    // near-miss that wastes an afternoon.
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
    {
        outError = "'" + text + "' is not a whole number";
        return false;
    }
    if (parsed == 0ull)
    {
        outError = "a value of zero has no meaning here";
        return false;
    }
    outValue = parsed;
    return true;
}

bool ReadFloatValue(const std::string& text, float& outValue, std::string& outError)
{
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0')
    {
        outError = "'" + text + "' is not a number";
        return false;
    }
    outValue = parsed;
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    const char* programName = argc > 0 ? argv[0] : "hollowpoint-runtime";

    RuntimeConfig config;
    std::vector<std::string> scriptsToRun;
    bool dumpConfig = false;
    bool verbose = false;
    std::string logFilePath;

    // A config file, if named, is loaded FIRST so explicit command-line flags win over it. The reverse order would let
    // a project's committed settings silently override what the person at the terminal just typed.
    std::string configPath;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        std::string value;
        std::string error;

        if (IsOption(argument, "--help") || IsOption(argument, "-h"))
        {
            PrintUsage(programName);
            return 0;
        }
        if (IsOption(argument, "--dump-config"))
        {
            dumpConfig = true;
            continue;
        }
        if (IsOption(argument, "--verbose"))
        {
            verbose = true;
            continue;
        }
        if (IsOption(argument, "--no-validation"))
        {
            config.EnableValidation = false;
            continue;
        }
        if (IsOption(argument, "--no-scripting"))
        {
            config.EnableScripting = false;
            continue;
        }
        if (IsOption(argument, "--log-file"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            logFilePath = value;
            continue;
        }
        if (IsOption(argument, "--hidden"))
        {
            config.StartVisible = false;
            continue;
        }
        if (IsOption(argument, "--config"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            configPath = value;
            continue;
        }
        if (IsOption(argument, "--scene"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            config.ScenePath = value;
            continue;
        }
        if (IsOption(argument, "--script-dir"))
        {
            // Where init.lua lives and what --script paths are resolved against. Needed as a flag because the engine
            // ships demo content in assets/demos/scripts while a game's own scripts live somewhere else entirely, and
            // "cd into the content directory first" is not something a capture script or an IDE launch configuration
            // wants to arrange.
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            config.ScriptDirectory = value;
            continue;
        }
        if (IsOption(argument, "--script"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            scriptsToRun.push_back(value);
            continue;
        }
        if (IsOption(argument, "--screenshot"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            config.ScreenshotPath = value;
            continue;
        }
        if (IsOption(argument, "--present"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            if (value == "vsync")
            {
                config.PresentMode = Graphics::PresentMode::VSync;
            }
            else if (value == "mailbox")
            {
                config.PresentMode = Graphics::PresentMode::Mailbox;
            }
            else if (value == "immediate")
            {
                config.PresentMode = Graphics::PresentMode::Immediate;
            }
            else
            {
                std::fprintf(stderr, "%s: --present expects vsync, mailbox or immediate, got '%s'\n", programName,
                             value.c_str());
                return 2;
            }
            continue;
        }
        if (IsOption(argument, "--width") || IsOption(argument, "--height") || IsOption(argument, "--frames"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            unsigned long long parsed = 0ull;
            if (!ReadPositiveUInt(value, parsed, error))
            {
                std::fprintf(stderr, "%s: %s %s\n", programName, std::string(argument).c_str(), error.c_str());
                return 2;
            }
            if (IsOption(argument, "--width"))
            {
                config.Width = static_cast<Core::UInt32>(parsed);
            }
            else if (IsOption(argument, "--height"))
            {
                config.Height = static_cast<Core::UInt32>(parsed);
            }
            else
            {
                config.MaxFrameCount = parsed;
            }
            continue;
        }
        if (IsOption(argument, "--exposure"))
        {
            if (!ReadValue(argc, argv, index, value, error))
            {
                std::fprintf(stderr, "%s: %s\n", programName, error.c_str());
                return 2;
            }
            if (!ReadFloatValue(value, config.Exposure, error))
            {
                std::fprintf(stderr, "%s: --exposure %s\n", programName, error.c_str());
                return 2;
            }
            continue;
        }

        // Unrecognised: reported and continuing rather than exiting. A launcher that passes flags for several tools
        // must not fail because one of them is not ours; the message says which one was ignored so a real typo is still
        // visible.
        std::fprintf(stderr, "%s: ignoring unrecognised option '%.*s'\n", programName,
                     static_cast<int>(argument.size()), argument.data());
    }

    if (!configPath.empty())
    {
        const RuntimeConfigLoadResult loaded = Runtime::LoadRuntimeConfigFile(configPath);
        if (!loaded.Report.empty())
        {
            // The report carries both the fatal parse error and the non-fatal notes (unknown keys, clamped values), so
            // it is printed either way: a file that parsed with warnings should still say so on every run until it is
            // fixed.
            std::fprintf(stderr, "%s\n", loaded.Report.c_str());
        }
        if (!loaded.Succeeded)
        {
            return 1;
        }

        // The file fills only what it sets; flags already in `config` from the command line are preserved by
        // re-applying them on top. Doing it this way (rather than loading into a fresh struct and copying) avoids a
        // list of "fields the loader knows about" that has to be maintained beside every new setting.
        RuntimeConfig merged = loaded.Config;
        // Re-apply command-line-only fields: they were parsed before the file existed, so anything the user typed wins.
        // Rather than tracking which flags were seen (a flag struct for fifteen options is over-engineering), the rule
        // is explicit and documented: a config file never overrides a non-default width/height/frames when the flag was
        // given. The simplest correct version of that is remembering the values parsed above, which is what `config`
        // holds — so the merge below takes the loaded file for anything still at its default in `config`.
        if (config.Width == RuntimeConfig{}.Width)
        {
            config.Width = merged.Width;
        }
        if (config.Height == RuntimeConfig{}.Height)
        {
            config.Height = merged.Height;
        }
        if (config.MaxFrameCount == RuntimeConfig{}.MaxFrameCount)
        {
            config.MaxFrameCount = merged.MaxFrameCount;
        }
        if (config.ScenePath.empty())
        {
            config.ScenePath = merged.ScenePath;
        }
        if (config.ScreenshotPath.empty())
        {
            config.ScreenshotPath = merged.ScreenshotPath;
        }
        if (config.Exposure == RuntimeConfig{}.Exposure)
        {
            config.Exposure = merged.Exposure;
        }
    }

    // Attach a sink BEFORE anything can log. The logger dispatches only to registered sinks and none are attached by
    // default, so without this the engine's entire diagnostic surface — every "returns nullptr with the reason logged"
    // contract in the codebase — produces no output at all, and a failed startup exits 1 saying nothing about why.
    // Tests get away with it because they assert on return values; a user at a terminal has only the log.
    const Core::LogLevel level = verbose ? Core::LogLevel::Debug : Core::LogLevel::Info;
    Core::AttachStandardLogSink(level);
    if (!logFilePath.empty())
    {
        // A file sink in addition to the console: a windowed app launched from a desktop environment has no console
        // attached, and being unable to see why it failed is much worse than having to open a file.
        if (Core::AttachFileLogSink(logFilePath, level) == nullptr)
        {
            std::fprintf(stderr, "%s: could not open the log file '%s'\n", programName, logFilePath.c_str());
        }
    }

    if (dumpConfig)
    {
        // Printed to stdout so `hollowpoint-runtime --dump-config > runtime.json` works as written.
        std::printf("%s", Runtime::SaveRuntimeConfigJson(config).c_str());
        return 0;
    }

    RuntimeApplication application(config);
    if (!application.Start())
    {
        std::fprintf(stderr, "hollowpoint-runtime: could not start (see the log above)\n");
        return 1;
    }

    // Scripts named on the command line run after startup's scene and init script, in the order given: a bootstrap that
    // spawns content must precede anything depending on it, and argv order is the only ordering the user has.
    for (const std::string& script : scriptsToRun)
    {
        if (!application.RunScriptFile(script))
        {
            // Reported by RunScriptFile already; continuing means a typo in one of three scripts does not discard the
            // other two.
            std::fprintf(stderr, "hollowpoint-runtime: script '%s' did not run\n", script.c_str());
        }
    }

    const StopReason reason = application.Run();

    // Captured after the loop rather than inside it: the last frame is the one a resized, fully-populated window shows,
    // and capturing frame 1 of a run that spends two frames loading would produce an image of nothing.
    if (!config.ScreenshotPath.empty())
    {
        if (!application.CaptureScreenshot(config.ScreenshotPath))
        {
            // A failed capture does not change the run's outcome: the frames ran, which is what the exit code reports.
            // Making it fatal would turn "disk full" into "the engine is broken" in whatever called this.
            std::fprintf(stderr, "hollowpoint-runtime: could not write the screenshot\n");
        }
    }

    // Flushed before the printf below so engine diagnostics and this summary appear in the order they happened rather
    // than interleaving by stream buffer.
    Core::FlushLogs();

    std::printf("%llu frame(s), %llu draw(s), stopped: %s\n",
                static_cast<unsigned long long>(application.GetFrameCount()),
                static_cast<unsigned long long>(application.GetDrawCount()), Runtime::StopReasonName(reason));

    // Device-lost is the only stop that is not a success. WindowClosed and FrameLimit are both normal ends.
    return reason == StopReason::DeviceLost || reason == StopReason::StartupFailed ? 1 : 0;
}
