#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Graphics/Swapchain.h"
#include "Hollowpoint/Runtime/RuntimeConfigLoader.h"
#include "Hollowpoint/Serialization/JsonParser.h"

// ---------------------------------------------------------------------------
// Loading a RuntimeConfig from JSON.
//
// Every field is read through one of the Read* helpers below, each of which reports what it found when the type was
// wrong rather than silently taking the default. That distinction matters: `{"width": "1280"}` (a string) and an absent
// `width` both end up with a default width, but only one of them is a mistake someone made in a file they will re-read
// tomorrow, and a config system that swallows the difference is why people stop trusting their config files.
//
// Unknown keys are reported but never fatal. A config written for a newer engine must still start; refusing to launch
// over an unrecognised word turns harmless forward-compatibility into a support ticket. The report keeps the typo
// visible.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Runtime
{

using Core::Int64;
using Core::ReadFileText;
using Core::Size;
using Serialization::JsonArray;
using Serialization::JsonDocument;
using Serialization::JsonObject;
using Serialization::JsonValue;
using Serialization::ParseJson;

namespace
{

//! Keys this version understands. Anything else is reported as unrecognised — a list rather than a switch so adding a
//! field means touching one place, and so the report can name what was close (a typo) when it can.
[[nodiscard]] const std::set<std::string, std::less<>>& KnownKeys()
{
    static const std::set<std::string, std::less<>> keys = {
        "applicationName", "width",      "height",          "resizable",       "startVisible",    "enableValidation",
        "presentMode",     "frameSlots", "maxLights",       "scenePath",       "scriptDirectory", "exposure",
        "background",      "maxFrames",  "idleWaitSeconds", "enableScripting", "screenshotPath",
    };
    return keys;
}

void AppendLine(std::string& report, std::string_view line)
{
    if (!report.empty())
    {
        report += "\n";
    }
    report += line;
}

[[nodiscard]] std::string Describe(const JsonValue& value)
{
    switch (value.GetKind())
    {
        case JsonValue::Kind::Null:
            return "null";
        case JsonValue::Kind::Boolean:
            return "a boolean";
        case JsonValue::Kind::Int:
        case JsonValue::Kind::UInt:
            return "an integer";
        case JsonValue::Kind::Real:
            return "a number";
        case JsonValue::Kind::String:
            return "a string";
        case JsonValue::Kind::Array:
            return "an array";
        case JsonValue::Kind::Object:
            return "an object";
    }
    return "an unexpected value";
}

//! Reads a string member. Reports when the key exists with the wrong type, and leaves `out` untouched then — so the
//! caller's default survives either way and only the diagnostic distinguishes the cases.
void ReadString(const JsonValue& root, const char* key, std::string& out, std::string& report)
{
    const JsonValue* node = root.FindMember(key);
    if (node == nullptr)
    {
        return;
    }
    if (node->GetKind() != JsonValue::Kind::String)
    {
        AppendLine(report, std::string("config key '") + key + "' is " + Describe(*node) +
                               ", expected a string; using the default");
        return;
    }
    out = node->AsString();
}

void ReadBool(const JsonValue& root, const char* key, bool& out, std::string& report)
{
    const JsonValue* node = root.FindMember(key);
    if (node == nullptr)
    {
        return;
    }
    if (node->GetKind() != JsonValue::Kind::Boolean)
    {
        AppendLine(report, std::string("config key '") + key + "' is " + Describe(*node) +
                               ", expected true or false; using the default");
        return;
    }
    out = node->AsBool();
}

//! Reads an integer member with a range check. Values outside the range are reported and rejected (default kept), not
//! clamped: `width: 0` clamped to 1 gives a 1-pixel window that looks like a rendering bug, while rejecting it keeps
//! the known-good default and says why in the log.
template <typename T>
void ReadInt(const JsonValue& root, const char* key, T& out, T minimum, bool allowZero, std::string& report)
{
    const JsonValue* node = root.FindMember(key);
    if (node == nullptr)
    {
        return;
    }
    if (node->GetKind() != JsonValue::Kind::Int && node->GetKind() != JsonValue::Kind::UInt)
    {
        AppendLine(report, std::string("config key '") + key + "' is " + Describe(*node) +
                               ", expected an integer; using the default");
        return;
    }

    const Int64 raw = node->AsInt();
    if (raw < static_cast<Core::Int64>(minimum) || (!allowZero && raw == 0))
    {
        AppendLine(report, std::string("config key '") + key + "' is " + std::to_string(raw) +
                               ", which is not a valid value; using the default");
        return;
    }

    out = static_cast<T>(raw);
}

void ReadFloat(const JsonValue& root, const char* key, float& out, std::string& report, bool nonNegative = false)
{
    const JsonValue* node = root.FindMember(key);
    if (node == nullptr)
    {
        return;
    }
    if (node->GetKind() != JsonValue::Kind::Real && node->GetKind() != JsonValue::Kind::Int &&
        node->GetKind() != JsonValue::Kind::UInt)
    {
        AppendLine(report, std::string("config key '") + key + "' is " + Describe(*node) +
                               ", expected a number; using the default");
        return;
    }

    const double raw = node->AsDouble();
    if (nonNegative && raw < 0.0)
    {
        AppendLine(report,
                   std::string("config key '") + key + "' is negative, which has no meaning here; using the default");
        return;
    }

    out = static_cast<float>(raw);
}

[[nodiscard]] Graphics::PresentMode ParsePresentMode(std::string_view text, bool& outValid)
{
    outValid = true;
    if (text == "vsync")
    {
        return Graphics::PresentMode::VSync;
    }
    if (text == "mailbox")
    {
        return Graphics::PresentMode::Mailbox;
    }
    if (text == "immediate")
    {
        return Graphics::PresentMode::Immediate;
    }

    outValid = false;
    return Graphics::PresentMode::VSync;
}

//! Reads `background` as either `[r,g,b]` or `[r,g,b,a]`. Alpha is accepted and ignored: the clear alpha a compositor
//! wants is an engine decision (opaque), so honouring it here would let a config produce a transparent window by
//! accident.
void ReadBackground(const JsonValue& root, RuntimeConfig& config, std::string& report)
{
    const JsonValue* node = root.FindMember("background");
    if (node == nullptr)
    {
        return;
    }
    if (!node->IsArray())
    {
        AppendLine(report, "config key 'background' is " + Describe(*node) +
                               ", expected an array of 3 numbers; using the default");
        return;
    }

    const Size count = node->GetElementCount();
    if (count < 3u)
    {
        AppendLine(report, "config key 'background' has " + std::to_string(count) +
                               " element(s); expected at least 3 (r, g, b); using the default");
        return;
    }

    float components[3] = {0.0f, 0.0f, 0.0f};
    for (Size index = 0; index < 3u; ++index)
    {
        const JsonValue* element = node->FindElement(index);
        if (element == nullptr ||
            (element->GetKind() != JsonValue::Kind::Real && element->GetKind() != JsonValue::Kind::Int &&
             element->GetKind() != JsonValue::Kind::UInt))
        {
            AppendLine(report, "config key 'background' element " + std::to_string(index) +
                                   " is not a number; using the default background");
            return;
        }
        components[index] = static_cast<float>(element->AsDouble());
    }

    if (count > 3u)
    {
        AppendLine(report, "config key 'background' supplies an alpha channel, which the clear colour ignores");
    }

    config.BackgroundRed = components[0];
    config.BackgroundGreen = components[1];
    config.BackgroundBlue = components[2];
}

void ReportUnknownKeys(const JsonValue& root, std::string& report)
{
    if (!root.IsObject())
    {
        return;
    }

    const JsonObject& object = root.AsObject();
    for (Size index = 0; index < object.GetCount(); ++index)
    {
        const std::string& key = object.GetKey(index);
        if (KnownKeys().count(key) == 0u)
        {
            AppendLine(report, "config key '" + key + "' is not recognised by this engine version and was ignored");
        }
    }
}

[[nodiscard]] std::string Quote(std::string_view text)
{
    std::string out = "\"";
    for (const char character : text)
    {
        if (character == '"' || character == '\\')
        {
            out += '\\';
        }
        out += character;
    }
    out += "\"";
    return out;
}

[[nodiscard]] const char* PresentModeName(Graphics::PresentMode mode) noexcept
{
    switch (mode)
    {
        case Graphics::PresentMode::VSync:
            return "vsync";
        case Graphics::PresentMode::Mailbox:
            return "mailbox";
        case Graphics::PresentMode::Immediate:
            return "immediate";
    }
    return "vsync";
}

}  // namespace

// --- RuntimeConfigLoadResult -------------------------------------------------

// --- entry points ------------------------------------------------------------

RuntimeConfigLoadResult ParseRuntimeConfigJson(std::string_view jsonText)
{
    RuntimeConfigLoadResult result;

    const JsonDocument document = ParseJson(jsonText);
    if (!document.Success)
    {
        result.Report = "configuration is not valid JSON: " + document.Error.Message;
        return result;
    }

    const JsonValue& root = document.Root;
    if (!root.IsObject())
    {
        // An array or scalar at the top level means the file is something else entirely (a scene, a stray paste).
        // Saying so beats reporting zero recognised keys, which reads as "the file was empty".
        result.Report = "configuration must be a JSON object, found " + Describe(root);
        return result;
    }

    RuntimeConfig& config = result.Config;

    ReadString(root, "applicationName", config.ApplicationName, result.Report);
    ReadInt(root, "width", config.Width, 1u, false, result.Report);
    ReadInt(root, "height", config.Height, 1u, false, result.Report);
    ReadBool(root, "resizable", config.Resizable, result.Report);
    ReadBool(root, "startVisible", config.StartVisible, result.Report);
    ReadBool(root, "enableValidation", config.EnableValidation, result.Report);
    ReadInt(root, "frameSlots", config.FrameSlots, 1u, false, result.Report);
    ReadInt(root, "maxLights", config.MaxLightsPerDraw, 1u, false, result.Report);
    ReadString(root, "scenePath", config.ScenePath, result.Report);
    ReadString(root, "scriptDirectory", config.ScriptDirectory, result.Report);
    ReadFloat(root, "exposure", config.Exposure, result.Report, /*nonNegative=*/true);
    ReadInt(root, "maxFrames", config.MaxFrameCount, static_cast<Core::UInt64>(0), true, result.Report);
    ReadBool(root, "enableScripting", config.EnableScripting, result.Report);
    ReadString(root, "screenshotPath", config.ScreenshotPath, result.Report);

    // idleWaitSeconds is a double (a duration compared against a double timeout), so it is read here rather than
    // through the float helper: going via float would round 0.05 to something that no longer compares equal to what the
    // file said, and a wait value is exactly the kind of number someone tunes in small increments.
    if (const JsonValue* idle = root.FindMember("idleWaitSeconds"); idle != nullptr)
    {
        if (idle->GetKind() == JsonValue::Kind::Real || idle->GetKind() == JsonValue::Kind::Int ||
            idle->GetKind() == JsonValue::Kind::UInt)
        {
            const double seconds = idle->AsDouble();
            if (seconds >= 0.0)
            {
                config.IdleWaitSeconds = seconds;
            }
            else
            {
                AppendLine(result.Report, "config key 'idleWaitSeconds' is negative; using the default");
            }
        }
        else
        {
            AppendLine(result.Report,
                       "config key 'idleWaitSeconds' is " + Describe(*idle) + ", expected a number; using the default");
        }
    }

    const JsonValue* presentMode = root.FindMember("presentMode");
    if (presentMode != nullptr)
    {
        if (presentMode->GetKind() != JsonValue::Kind::String)
        {
            AppendLine(result.Report, "config key 'presentMode' is " + Describe(*presentMode) +
                                          ", expected a string; using the default");
        }
        else
        {
            bool valid = false;
            config.PresentMode = ParsePresentMode(presentMode->AsString(), valid);
            if (!valid)
            {
                AppendLine(result.Report, "config key 'presentMode' is '" + presentMode->AsString() +
                                              "', expected one of vsync, mailbox, immediate; using the default");
            }
        }
    }

    ReadBackground(root, config, result.Report);
    ReportUnknownKeys(root, result.Report);

    // Window sizes are content pixels; a HiDPI display multiplies them. Nothing to validate beyond the range check
    // above — but a frame-slot count below 2 is worth saying something about, since it serialises CPU and GPU. Not an
    // error, just a note: someone benchmarking with FrameSlots=1 should be able to tell later that this was why the
    // numbers were odd.
    if (config.FrameSlots == 1u)
    {
        AppendLine(result.Report, "frameSlots is 1: CPU and GPU will not overlap");
    }

    result.Succeeded = true;
    return result;
}

RuntimeConfigLoadResult LoadRuntimeConfigFile(const std::string& path)
{
    const auto file = ReadFileText(path);
    if (!file.Succeeded)
    {
        RuntimeConfigLoadResult result;
        // Carrying the classified reason is the point: "could not read config" and "config does not exist" lead to
        // different next actions, and Core::FileSystem already distinguishes them.
        result.Report = "configuration file '" + path + "' could not be read: " + file.Message;
        return result;
    }

    RuntimeConfigLoadResult result = ParseRuntimeConfigJson(file.Value);
    if (!result.Succeeded)
    {
        // Prefix with the path: a project can have several config files (per platform, per preset), and an error naming
        // only the JSON content gives no way to tell which one broke.
        result.Report = "'" + path + "': " + result.Report;
    }
    return result;
}

std::string SaveRuntimeConfigJson(const RuntimeConfig& config)
{
    // Hand-rendered rather than using a JsonValue builder: the output is a documented example people read, so key order
    // and grouping are chosen for that reader (window, then presentation, then content), which a map-backed builder
    // would sort alphabetically and scatter. Values still come from the struct, so defaults cannot drift from what the
    // engine uses.
    std::ostringstream out;
    out << "{\n";
    out << "  \"applicationName\": " << Quote(config.ApplicationName) << ",\n";
    out << "  \"width\": " << config.Width << ",\n";
    out << "  \"height\": " << config.Height << ",\n";
    out << "  \"resizable\": " << (config.Resizable ? "true" : "false") << ",\n";
    out << "  \"startVisible\": " << (config.StartVisible ? "true" : "false") << ",\n";
    out << "  \"enableValidation\": " << (config.EnableValidation ? "true" : "false") << ",\n";
    out << "  \"presentMode\": " << Quote(PresentModeName(config.PresentMode)) << ",\n";
    out << "  \"frameSlots\": " << config.FrameSlots << ",\n";
    out << "  \"maxLights\": " << config.MaxLightsPerDraw << ",\n";
    out << "  \"exposure\": " << config.Exposure << ",\n";
    out << "  \"background\": [" << config.BackgroundRed << ", " << config.BackgroundGreen << ", "
        << config.BackgroundBlue << "],\n";
    out << "  \"scenePath\": " << Quote(config.ScenePath) << ",\n";
    out << "  \"scriptDirectory\": " << Quote(config.ScriptDirectory) << ",\n";
    out << "  \"enableScripting\": " << (config.EnableScripting ? "true" : "false") << ",\n";
    out << "  \"maxFrames\": " << config.MaxFrameCount << ",\n";
    out << "  \"screenshotPath\": " << Quote(config.ScreenshotPath) << "\n";
    out << "}\n";
    return out.str();
}

}  // namespace Hollowpoint::Runtime
