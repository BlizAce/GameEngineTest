#include "GlslangInclude.h"

#include <cctype>
#include <mutex>
#include <string>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"

namespace Hollowpoint::Graphics
{

using Core::Size;

// glslang spreads its API across the glslang namespace and the global namespace inconsistently: EShLanguage and
// EShMessages are declared globally, while EShSource*/EShClient*/TShader/TProgram live in namespace glslang. Listing
// individual using-declarations for that mix is error-prone (each one needs the right qualification, and guessing
// wrong is a compile error per name), so the whole namespace is imported inside this translation unit only. It is
// confined to a .cpp, and no engine identifier used here collides with a glslang name.
using namespace glslang;  // NOLINT(google-build-using-namespace): .cpp-only, see above.

namespace
{

// glslang initialisation is reference-counted process-wide, and every compile takes this lock. glslang documents
// per-thread initialisation but keeps process-global caches (the resource-limits table, string interning), so the
// safe contract is one compile at a time. Compilation happens at load/editor time rather than per frame, so this
// is never on a hot path — winning parallelism here would mean auditing glslang's internals, not worth the risk.
std::mutex g_CompilerMutex;
int g_ScopeCount = 0;

[[nodiscard]] EShLanguage ToGlslangStage(ShaderStage stage) noexcept
{
    switch (stage)
    {
        case ShaderStage::Vertex:
            return EShLangVertex;
        case ShaderStage::Pixel:
            return EShLangFragment;
        case ShaderStage::Compute:
            return EShLangCompute;
    }

    // Unreachable for valid inputs. Returning Vertex rather than aborting keeps the function total; a wrong stage
    // then surfaces as "no main entry point", which is at least a compile error and not memory corruption.
    HP_LOG_ERROR("GRAPHICS", "CompileGlsl called with an invalid shader stage value ({})", static_cast<int>(stage));
    return EShLangVertex;
}

//! RAII ownership of glslang process initialisation, so no early-return path can leak it — a leaked initialisation
//! poisons every later compile and skips FinalizeProcess at shutdown.
class GlslangProcessGuard
{
public:
    //! Initialises when this is the first user. Callers hold g_CompilerMutex, which this class does not take itself
    //! (it is constructed and destroyed inside a single locked region).
    GlslangProcessGuard()
    {
        if (g_ScopeCount == 0)
        {
            InitializeProcess();
        }
        ++g_ScopeCount;
    }

    ~GlslangProcessGuard()
    {
        if (--g_ScopeCount == 0)
        {
            FinalizeProcess();
        }
    }

    GlslangProcessGuard(const GlslangProcessGuard&) = delete;
    GlslangProcessGuard& operator=(const GlslangProcessGuard&) = delete;
};

//! Extracts a 1-based line number from glslang diagnostics, whose errors look like
//! "ERROR: <source name>:<line>: '<token>' : <message>".
//!
//! The scan looks for a ":<digits>:" run rather than the first colon, because the text begins with an "ERROR: " or
//! "WARNING: " prefix (whose colon is followed by a space) and the source name may itself contain colons — an
//! absolute path on Windows, or a shader-cache key. Returns 0 when no line can be identified: the number only drives
//! editor navigation, so failing to find it must not produce a wrong one.
[[nodiscard]] UInt32 ParseFirstErrorLine(const std::string& diagnostics)
{
    for (Size index = 0; index + 2 < diagnostics.size(); ++index)
    {
        if (diagnostics[index] != ':' || !std::isdigit(static_cast<unsigned char>(diagnostics[index + 1])))
        {
            continue;
        }

        Size end = index + 1;
        while (end < diagnostics.size() && std::isdigit(static_cast<unsigned char>(diagnostics[end])))
        {
            ++end;
        }

        // Require a closing colon so a bare number in prose ("expected 2 arguments") is not mistaken for a line.
        if (end >= diagnostics.size() || diagnostics[end] != ':')
        {
            index = end;
            continue;
        }

        UInt32 line = 0;
        for (Size digit = index + 1; digit < end; ++digit)
        {
            line = line * 10u + static_cast<UInt32>(diagnostics[digit] - '0');
        }

        if (line > 0u)
        {
            return line;
        }
        index = end;
    }

    return 0;
}

//! Shared failure recording so every error path logs and reports identically. SPIR-V is deliberately left empty: the
//! documented contract is that a failed compile carries no module, so a caller that ignores Succeeded cannot end up
//! rendering with a shader that was rejected (by policy or otherwise).
[[nodiscard]] CompiledShader Fail(ShaderStage stage, const std::string& sourceName, std::string diagnostics)
{
    CompiledShader result;
    result.Succeeded = false;
    result.Diagnostics = std::move(diagnostics);
    result.ErrorLine = ParseFirstErrorLine(result.Diagnostics);

    HP_LOG_ERROR("GRAPHICS", "Compiling {} shader '{}' failed:\n{}", ShaderStageName(stage), sourceName,
                 result.Diagnostics);
    return result;
}

}  // namespace

const char* ShaderStageName(ShaderStage stage) noexcept
{
    switch (stage)
    {
        case ShaderStage::Vertex:
            return "vertex";
        case ShaderStage::Pixel:
            return "pixel";
        case ShaderStage::Compute:
            return "compute";
    }
    return "unknown";
}

ShaderCompilerScope::ShaderCompilerScope()
{
    std::lock_guard<std::mutex> lock(g_CompilerMutex);

    if (g_ScopeCount == 0)
    {
        InitializeProcess();
    }
    ++g_ScopeCount;
}

ShaderCompilerScope::~ShaderCompilerScope()
{
    std::lock_guard<std::mutex> lock(g_CompilerMutex);

    --g_ScopeCount;
    if (g_ScopeCount <= 0)
    {
        // Clamp so a mismatched destruction sequence cannot go negative and then re-initialise on the next scope
        // while glslang is already finalised.
        g_ScopeCount = 0;
        FinalizeProcess();
    }
}

bool ShaderCompilerScope::IsInitialized() const noexcept
{
    return g_ScopeCount > 0;
}

int ShaderCompilerScope::GetActiveScopeCount() noexcept
{
    std::lock_guard<std::mutex> lock(g_CompilerMutex);
    return g_ScopeCount;
}

CompiledShader CompileGlsl(ShaderStage stage, const std::string& source, const ShaderCompileOptions& options)
{
    // Empty source is a caller error (typically a failed file read arriving here). glslang's answer is an unhelpful
    // parse error at line 0, so the real cause is reported instead.
    if (source.empty())
    {
        return Fail(stage, options.SourceName, "Shader source is empty");
    }

    std::lock_guard<std::mutex> lock(g_CompilerMutex);

    // Compiling without an outer ShaderCompilerScope is supported: the guard initialises and releases glslang for
    // this one call. A long-lived scope amortises that when many shaders load in a row.
    const GlslangProcessGuard processGuard;

    // SPIR-V generation plus the Vulkan GLSL rules (no implicit conversions, explicit I/O layouts). The client and
    // target are declared per-shader below; these flags control diagnostics and code generation.
    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (options.GenerateDebugInfo)
    {
        messages = static_cast<EShMessages>(messages | EShMsgDebugInfo);
    }

    // glslang's "default" resource table describes a permissive implementation. Its strict built-in defaults reject
    // constructs every real Vulkan driver accepts, which would fail shaders for reasons no target device would.
    const TBuiltInResource* resources = GetDefaultResources();

    const EShLanguage language = ToGlslangStage(stage);

    TShader shader(language);
    const char* sourcePointer = source.c_str();
    const int sourceLength = static_cast<int>(source.size());
    const char* namePointer = options.SourceName.c_str();

    // Length-carrying form: sources often come straight from a file buffer and are not NUL-terminated, so relying
    // on that would be a latent overrun.
    shader.setStringsWithLengthsAndNames(&sourcePointer, &sourceLength, &namePointer, 1);

    // Declare the target environment: GLSL source with Vulkan client semantics (the KHR_vulkan_glsl extension, which
    // turns samplers/opaque types into descriptors), producing SPIR-V for Vulkan 1.2. glslangValidator does this
    // too; omitting it silently compiles desktop GLSL and emits no SPIR-V, surfacing much later as an empty module.
    shader.setEnvInput(EShSourceGlsl, language, EShClientVulkan, 100);
    shader.setEnvClient(EShClientVulkan, EShTargetVulkan_1_2);
    shader.setEnvTarget(EShTargetSpv, EShTargetSpv_1_5);

    if (!options.EntryPoint.empty() && options.EntryPoint != "main")
    {
        // GLSL fixes the entry point to `main` per stage; renaming applies to HLSL. Forwarding it for GLSL would
        // make glslang search for a function that cannot exist and emit a misleading error.
        shader.setEntryPoint(options.EntryPoint.c_str());
        shader.setSourceEntryPoint(options.EntryPoint.c_str());
    }

    if (!shader.parse(resources, 100, ENoProfile, false, false, messages))
    {
        return Fail(stage, options.SourceName, shader.getInfoLog());
    }

    // Diagnostics exist even on success: warnings are the interesting part of a "successful" compile, because a
    // shader that warns usually relies on undefined behaviour that differs between drivers.
    const std::string diagnostics = shader.getInfoLog();

    // Linking produces reflection data and the final module; a one-shader program is exactly how a single stage
    // becomes a standalone SPIR-V binary.
    TProgram program;
    program.addShader(&shader);

    if (!program.link(messages))
    {
        return Fail(stage, options.SourceName, program.getInfoLog());
    }

    const TIntermediate* intermediate = program.getIntermediate(language);
    if (intermediate == nullptr)
    {
        return Fail(stage, options.SourceName, "No intermediate representation after linking");
    }

    SpvOptions spvOptions;
    spvOptions.generateDebugInfo = options.GenerateDebugInfo;
    // SPIRV-Tools is skipped entirely unless the optimiser is enabled, so this is the switch that actually turns it
    // on. Size and performance pass sets are chosen from that rather than a numeric level, because that is what
    // glslang exposes — a 0..3 "level" option here would silently do nothing at some settings.
    spvOptions.disableOptimizer = !options.Optimize;
    spvOptions.optimizeSize = options.Optimize && options.OptimizeForSize;
    spvOptions.optimizePerformance = options.Optimize && !options.OptimizeForSize;
    // Validate the module we emit: malformed SPIR-V would otherwise survive until the driver rejects it during
    // pipeline creation, far from the shader that caused it.
    spvOptions.validate = true;

    CompiledShader result;
    GlslangToSpv(*intermediate, result.SpirV, &spvOptions);

    if (result.SpirV.empty())
    {
        return Fail(stage, options.SourceName,
                    diagnostics.empty() ? "SPIR-V generation produced no output" : diagnostics);
    }

    const bool hasWarnings = !diagnostics.empty();

    if (options.WarningsAsErrors && hasWarnings)
    {
        // The module compiled, but the policy rejects it. Returning the SPIR-V anyway would break the documented
        // contract ("a failed compile carries no module") and let a caller that checks Succeeded inconsistently —
        // or not at all — upload a shader precisely because it warned. Diagnostics are preserved either way.
        return Fail(stage, options.SourceName, diagnostics);
    }

    result.Diagnostics = diagnostics;
    result.Succeeded = true;

    if (hasWarnings)
    {
        HP_LOG_WARNING("GRAPHICS", "Compiling {} shader '{}':\n{}", ShaderStageName(stage), options.SourceName,
                       diagnostics);
    }

    return result;
}

CompiledShader CompileGlslFromFileContents(ShaderStage stage, const std::string& source, const std::string& displayName)
{
    ShaderCompileOptions options;
    options.SourceName = displayName;
    return CompileGlsl(stage, source, options);
}

}  // namespace Hollowpoint::Graphics
