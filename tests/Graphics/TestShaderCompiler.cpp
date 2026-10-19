#include "TestSupport.h"

#include <array>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Graphics/ShaderCompiler.h"

using namespace Hollowpoint::Graphics;

namespace
{

// ---------------------------------------------------------------------------
// Shader sources.
//
// Kept as raw literals close to what a real asset file contains, because the thing under test is the compiler
// front end: an over-synthesised input (no #version, no layout qualifiers) would exercise a path that no shipped
// shader uses and pass while real shaders failed.
// ---------------------------------------------------------------------------

constexpr const char* kVertex = R"glsl(#version 450
layout(location = 0) in vec3 InPosition;
layout(location = 1) in vec2 InUv;
layout(location = 0) out vec2 OutUv;
layout(set = 0, binding = 0) uniform Globals {
    mat4 ViewProj;
} globals;
void main() {
    OutUv = InUv;
    gl_Position = globals.ViewProj * vec4(InPosition, 1.0);
}
)glsl";

constexpr const char* kFragment = R"glsl(#version 450
layout(location = 0) in vec2 InUv;
layout(location = 0) out vec4 OutColor;
void main() { OutColor = vec4(InUv, 0.5, 1.0); }
)glsl";

constexpr const char* kCompute = R"glsl(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) buffer Data { float values[]; } data;
void main() { data.values[gl_GlobalInvocationID.x] = 1.0; }
)glsl";

//! A type error on line 3 — the line number is asserted, so this comment's own position matters not at all, but
//! the source layout does: keep the error exactly three lines down.
constexpr const char* kBrokenVertex = R"glsl(#version 450
void main() {
    vec4 p = notAFunction();
    gl_Position = p;
}
)glsl";

//! No #version at all. Vulkan requires 310+, and glslang's message is the only clue a user gets.
constexpr const char* kMissingVersion = "void main() { gl_Position = vec4(0.0); }\n";

//! Compiles, but warns: an unsupported extension. Exercises the warnings-as-errors policy without being a hard
//! error, which a test needs to distinguish "policy rejected" from "compiler rejected".
constexpr const char* kWarningVertex = R"glsl(#version 450
#extension GL_HOLLOWPOINT_definitely_not_real : warn
void main() { gl_Position = vec4(0.0); }
)glsl";

[[nodiscard]] bool IsSpirV(const std::vector<Hollowpoint::Core::UInt32>& words)
{
    // SPIR-V module magic number, low byte first: 0x07230203.
    return !words.empty() && words[0] == 0x07230203u;
}

[[nodiscard]] Hollowpoint::Core::UInt32 SpirVVersion(const std::vector<Hollowpoint::Core::UInt32>& words)
{
    if (words.size() < 2)
    {
        return 0;
    }
    return words[1];  // major.minor in the high two bytes.
}

}  // namespace

TEST_SUITE("Graphics/ShaderCompiler")
{
    TEST_CASE("a vertex shader compiles to SPIR-V")
    {
        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kVertex);

        INFO(shader.Diagnostics);
        CHECK(shader.Succeeded);
        CHECK(IsSpirV(shader.SpirV));
        CHECK_FALSE(shader.SpirV.empty());
        CHECK(shader.ErrorLine == 0u);
    }

    TEST_CASE("fragment and compute stages compile")
    {
        const CompiledShader fragment = CompileGlsl(ShaderStage::Pixel, kFragment);
        INFO(fragment.Diagnostics);
        CHECK(fragment.Succeeded);
        CHECK(IsSpirV(fragment.SpirV));

        const CompiledShader compute = CompileGlsl(ShaderStage::Compute, kCompute);
        INFO(compute.Diagnostics);
        CHECK(compute.Succeeded);
        CHECK(IsSpirV(compute.SpirV));
    }

    TEST_CASE("stages produce different modules")
    {
        // Guards against a stage-mapping bug where every stage compiles as the same one: that produces valid SPIR-V
        // which then fails at pipeline creation with an unrelated-looking error.
        const CompiledShader vertex = CompileGlsl(ShaderStage::Vertex, kVertex);
        const CompiledShader fragment = CompileGlsl(ShaderStage::Pixel, kFragment);

        REQUIRE(vertex.Succeeded);
        REQUIRE(fragment.Succeeded);
        CHECK(vertex.SpirV != fragment.SpirV);
    }

    TEST_CASE("output declares the SPIR-V version we target")
    {
        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kVertex);
        REQUIRE(shader.Succeeded);

        // SPIR-V 1.5: major in bits 16-23, minor in 8-15. Targeting 1.5 is what allows the descriptor indexing and
        // separate-storage-class features later passes rely on.
        CHECK(SpirVVersion(shader.SpirV) == 0x00010500u);
    }

    TEST_CASE("a compile error is reported, not thrown, and names the line")
    {
        // Shader errors are routine (editing), so this must be data rather than an exception: an editor has to show
        // the message and keep running with the previous shader.
        Hollowpoint::Test::ScopedLogCapture logs;

        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kBrokenVertex);

        CHECK_FALSE(shader.Succeeded);
        CHECK(shader.SpirV.empty());  // A rejected compile must carry no module.
        CHECK_FALSE(shader.Diagnostics.empty());
        CHECK(shader.ErrorLine == 3u);  // The line with the bad call.
        CHECK(logs.GetErrorCount() >= 1);
    }

    TEST_CASE("a missing #version fails with an actionable message")
    {
        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kMissingVersion);

        CHECK_FALSE(shader.Succeeded);
        CHECK(shader.SpirV.empty());
        // glslang's own wording; asserted because it is the only hint a user gets about Vulkan requiring 310+.
        CHECK(shader.Diagnostics.find("version") != std::string::npos);
    }

    TEST_CASE("warnings do not fail by default and are surfaced")
    {
        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kWarningVertex);

        CHECK(shader.Succeeded);
        CHECK(IsSpirV(shader.SpirV));
        CHECK_FALSE(shader.Diagnostics.empty());  // The warning is reported even though compilation succeeded.
    }

    TEST_CASE("warnings-as-errors rejects a warninging shader and returns no module")
    {
        ShaderCompileOptions options;
        options.WarningsAsErrors = true;

        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kWarningVertex, options);

        CHECK_FALSE(shader.Succeeded);
        // The documented contract is that a failed compile carries no SPIR-V. Without this, a caller that checks the
        // flag inconsistently could upload a shader precisely because it warned.
        CHECK(shader.SpirV.empty());
        CHECK(shader.ErrorLine == 2u);
    }

    TEST_CASE("warnings-as-errors leaves a clean shader compiling")
    {
        ShaderCompileOptions options;
        options.WarningsAsErrors = true;

        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kVertex, options);

        INFO(shader.Diagnostics);
        CHECK(shader.Succeeded);
        CHECK(IsSpirV(shader.SpirV));
    }

    TEST_CASE("debug info changes the module size")
    {
        ShaderCompileOptions withDebug;
        withDebug.GenerateDebugInfo = true;

        ShaderCompileOptions withoutDebug;
        withoutDebug.GenerateDebugInfo = false;

        const CompiledShader debug = CompileGlsl(ShaderStage::Vertex, kVertex, withDebug);
        const CompiledShader release = CompileGlsl(ShaderStage::Vertex, kVertex, withoutDebug);

        REQUIRE(debug.Succeeded);
        REQUIRE(release.Succeeded);

        // Debug builds carry OpName/OpLine for renderdoc. Asserting the direction (rather than exact sizes, which
        // change with glslang) is what catches the flag being ignored or inverted.
        CHECK(debug.SpirV.size() > release.SpirV.size());
    }

    TEST_CASE("the optimiser runs when asked and produces a valid module")
    {
        ShaderCompileOptions options;
        options.Optimize = true;

        const CompiledShader optimized = CompileGlsl(ShaderStage::Vertex, kVertex, options);

        INFO(optimized.Diagnostics);
        CHECK(optimized.Succeeded);
        CHECK(IsSpirV(optimized.SpirV));
    }

    TEST_CASE("empty source is rejected with a clear message")
    {
        // Typically a failed file read arriving here. glslang's own answer would be an unhelpful parse error at line
        // 0, so the real cause must be stated.
        Hollowpoint::Test::ScopedLogCapture logs;

        const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, std::string());

        CHECK_FALSE(shader.Succeeded);
        CHECK(shader.SpirV.empty());
        CHECK(shader.Diagnostics.find("empty") != std::string::npos);
        CHECK(logs.GetErrorCount() >= 1);
    }

    TEST_CASE("the source name appears in diagnostics")
    {
        // Without it, an error says "line 3" of an unnamed file and cannot be acted on from a log alone.
        const CompiledShader shader =
            CompileGlslFromFileContents(ShaderStage::Vertex, kBrokenVertex, "shaders/player.vert");

        CHECK_FALSE(shader.Succeeded);
        CHECK(shader.Diagnostics.find("shaders/player.vert") != std::string::npos);
    }

    TEST_CASE("stage names are readable for logs")
    {
        CHECK(ShaderStageName(ShaderStage::Vertex) == std::string("vertex"));
        CHECK(ShaderStageName(ShaderStage::Pixel) == std::string("pixel"));
        CHECK(ShaderStageName(ShaderStage::Compute) == std::string("compute"));
    }

    TEST_CASE("compiling many shaders in a row stays consistent")
    {
        // Catches glslang state leaking between compiles — the failure mode of the process-scope refcounting, where
        // a missing FinalizeProcess makes later compiles behave differently from the first.
        for (int iteration = 0; iteration < 25; ++iteration)
        {
            const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kVertex);
            INFO("iteration " << iteration);
            REQUIRE(shader.Succeeded);
            CHECK(IsSpirV(shader.SpirV));

            // Same input must produce byte-identical output; a difference would mean unreproducible builds.
            if (iteration > 0)
            {
                const CompiledShader again = CompileGlsl(ShaderStage::Vertex, kVertex);
                CHECK(shader.SpirV == again.SpirV);
            }
        }
    }

    TEST_CASE("an explicit scope nests with per-call compilation")
    {
        const int before = ShaderCompilerScope::GetActiveScopeCount();

        {
            const ShaderCompilerScope outer;
            CHECK(outer.IsInitialized());
            CHECK(ShaderCompilerScope::GetActiveScopeCount() == before + 1);

            // Compilation inside an explicit scope must work and must not disturb the count.
            const CompiledShader shader = CompileGlsl(ShaderStage::Vertex, kVertex);
            CHECK(shader.Succeeded);
            CHECK(ShaderCompilerScope::GetActiveScopeCount() == before + 1);

            {
                const ShaderCompilerScope inner;
                CHECK(ShaderCompilerScope::GetActiveScopeCount() == before + 2);
            }

            CHECK(ShaderCompilerScope::GetActiveScopeCount() == before + 1);
        }

        CHECK(ShaderCompilerScope::GetActiveScopeCount() == before);

        // And glslang still works after every scope has been released (it re-initialises per call).
        const CompiledShader after = CompileGlsl(ShaderStage::Vertex, kVertex);
        CHECK(after.Succeeded);
    }
}
