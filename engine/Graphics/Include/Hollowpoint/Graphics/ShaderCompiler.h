#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Graphics/GraphicsApi.h"

namespace Hollowpoint::Graphics
{

// ---------------------------------------------------------------------------
// Offline GLSL -> SPIR-V compilation.
//
// Shaders are compiled at load time rather than shipped as precompiled SPIR-V for two reasons. Editor
// iteration: a shader edit should show up on the next frame, not require an asset-bake step that is easy to
// forget. Portability: SPIR-V is portable across drivers in practice but vendor behaviour on unusual
// constructs differs, and compiling with our own glslang build gives one deterministic diagnosis for every
// machine rather than "driver X rejected it" reports we cannot reproduce.
//
// Compilation happens through glslang's C++ API rather than by spawning `glslangValidator`: a subprocess
// costs ~50 ms of process startup per shader (which adds up when an editor recompiles a pipeline variant on
// every keystroke), gives us only a command line and an exit code to diagnose with, and would have to be
// shipped alongside the engine. Linking glslang means compile errors come back as structured data in-process.
//
// Thread safety: glslang requires per-thread initialisation (InitializeProcess/FinalizeProcess). Compilation
// is therefore guarded by a process-wide scope object rather than each call doing its own, so concurrent
// shader loads from an asset worker pool are safe without paying for re-initialisation per shader.
// ---------------------------------------------------------------------------

//! Which pipeline stage a shader source implements. Mirrors NVRHI's ShaderType but stays independent of it so
//! tools can compile shaders without linking the RHI.
enum class ShaderStage : UInt8
{
    Vertex,
    Pixel,
    Compute,
};

//! Human-readable stage name ("vertex"), for log lines and error messages. Never null.
[[nodiscard]] HP_GRAPHICS_API const char* ShaderStageName(ShaderStage stage) noexcept;

//! Result of compiling one shader stage.
struct CompiledShader
{
    //! SPIR-V words, ready to hand to NVRHI's createShader. Empty when Succeeded is false.
    std::vector<UInt32> SpirV;

    //! True when the shader compiled and produced code.
    bool Succeeded = false;

    //! Compiler diagnostics (errors, or warnings when configured to report them). Kept even on success because
    //! a shader that compiles with warnings is usually relying on undefined behaviour.
    std::string Diagnostics;

    //! Source line the first error refers to (1-based), or 0 when unavailable. Surfaced separately from the
    //! message so an editor can jump straight there instead of parsing glslang's text format, which varies
    //! between releases.
    UInt32 ErrorLine = 0;
};

//! Options for a compile request.
struct ShaderCompileOptions
{
    //! Entry point name. "main" for GLSL (where the entry point is fixed by stage); matters for shared
    //! sources with several stages and for HLSL.
    std::string EntryPoint = "main";

    //! Name used in diagnostics and embedded debug names. Should be a file path relative to the project so an
    //! error message identifies the file without an absolute path leaking into logs.
    std::string SourceName = "<unnamed shader>";

    //! Emit SPIR-V debug information ( OpName / OpLine ). True in editor builds so renderdoc and GPU vendors'
    //! tools can name variables; false in shipping builds where it is dead weight in every pipeline state.
    bool GenerateDebugInfo = true;

    //! Treat warnings as errors. Editor default should be true: a shader that warns today produces wrong
    //! pixels tomorrow, and the cost of fixing a warning now is trivial compared to bisecting a visual bug
    //! caused by it later.
    bool WarningsAsErrors = false;

    //! Optimise the generated SPIR-V with SPIRV-Tools (performance passes). Off by default: optimisation can
    //! reorder floating-point operations and therefore change pixel output, so a shader that must reproduce a
    //! reference image is built unoptimised and production builds opt in deliberately once results are known-good.
    bool Optimize = false;

    //! Optimise for size rather than speed (SPIRV-Tools size passes). Only meaningful with Optimize true. Chosen
    //! as a separate flag because glslang exposes the two as different pass sets, not a level: there is no 0..3
    //! dial to expose, and pretending there were would be an option that silently does nothing at some settings.
    bool OptimizeForSize = false;
};

//! Compiles GLSL source to SPIR-V for one stage.
//!
//! Never throws: a shader that fails to compile is an expected, routine event (a typo while editing), and the
//! caller must be able to display the error and continue running with the previous shader. Failures are
//! reported through CompiledShader::Succeeded and ::Diagnostics.
[[nodiscard]] HP_GRAPHICS_API CompiledShader CompileGlsl(ShaderStage stage, const std::string& source,
                                                         const ShaderCompileOptions& options = {});

//! Convenience for the common editor case: compile with the default options and a name taken from a file path.
[[nodiscard]] HP_GRAPHICS_API CompiledShader CompileGlslFromFileContents(ShaderStage stage, const std::string& source,
                                                                         const std::string& displayName);

// ---------------------------------------------------------------------------
// glslang process scope.
//
// glslang keeps global state (the built-in resource limits table, thread-local pools) that must be
// initialised before use and released afterwards. Reference counting matches GLFW's model in Platform: the
// first scope initialises, the last releases, and nested scopes are free. Compiling without a live scope is
// not checked portably by glslang itself, so this wrapper is what makes "who initialised glslang?" answerable.
// ---------------------------------------------------------------------------

class HP_GRAPHICS_API ShaderCompilerScope
{
public:
    //! Initialises glslang for the current thread (reference-counted process-wide).
    ShaderCompilerScope();
    ~ShaderCompilerScope();

    ShaderCompilerScope(const ShaderCompilerScope&) = delete;
    ShaderCompilerScope& operator=(const ShaderCompilerScope&) = delete;
    ShaderCompilerScope(ShaderCompilerScope&&) = delete;
    ShaderCompilerScope& operator=(ShaderCompilerScope&&) = delete;

    [[nodiscard]] bool IsInitialized() const noexcept;

    [[nodiscard]] static int GetActiveScopeCount() noexcept;
};

}  // namespace Hollowpoint::Graphics
