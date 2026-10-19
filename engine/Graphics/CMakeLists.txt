# Hollowpoint Graphics: GPU device, resources and offline shader compilation.
#
# Wraps NVRHI (and through it Vulkan) behind engine-level types. Two design rules govern this module:
#
#   * Public headers include neither Vulkan nor NVRHI. Both are huge and both would leak the RHI's vocabulary
#     into everything above; they appear only in Src via a private internal header. Tools that only need shader
#     compilation therefore link nothing GPU-related.
#   * The engine owns the native VkInstance/VkDevice. NVRHI's Vulkan backend deliberately does not create them, so
#     device selection, extension lists and surface lifetime are ours — see GraphicsDevice.

add_library(HollowpointGraphics STATIC)
hp_set_engine_target(HollowpointGraphics)

target_include_directories(HollowpointGraphics PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointGraphics PRIVATE
    Include/Hollowpoint/Graphics/GraphicsApi.h
    Include/Hollowpoint/Graphics/ShaderCompiler.h
    Src/ShaderCompiler.cpp)

# glslang is linked for offline GLSL -> SPIR-V compilation. Its own headers are exposed as SYSTEM includes so its
# internals do not trip the engine's warning set (the source file also scopes a suppression, belt and braces).
target_link_libraries(HollowpointGraphics PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::CompilerSetup
    glslang
    glslang-default-resource-limits
    SPIRV
    SPIRV-Tools-opt)

add_library(Hollowpoint::Graphics ALIAS HollowpointGraphics)
