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
    Include/Hollowpoint/Graphics/GraphicsDevice.h
    Include/Hollowpoint/Graphics/RhiAccess.h
    Include/Hollowpoint/Graphics/ShaderCompiler.h
    Src/GlslangInclude.h
    Src/GraphicsDevice.cpp
    Src/ShaderCompiler.cpp
    Src/VulkanDispatcher.cpp)

# glslang is linked for offline GLSL -> SPIR-V compilation. Its own headers are exposed as SYSTEM includes so its
# internals do not trip the engine's warning set (the source file also scopes a suppression, belt and braces).
# Platform is needed for AttachWindow (creating a VkSurfaceKHR from a window). The direction is deliberate:
# Platform knows nothing about rendering, so Graphics depending on it cannot create a cycle.
#
# hp_shaderc and hp_vulkan_loader are interface wrappers created in third_party/ — the glslang/SPIRV targets are
# IMPORTED-style targets defined there and do not propagate to sibling directories (the same reason Platform links
# hp_glfw rather than glfw). The loader wrapper also works around this system having no `libvulkan.so` development
# symlink: it carries the resolved absolute path to libvulkan.so.1.
target_link_libraries(HollowpointGraphics PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::Platform
    Hollowpoint::CompilerSetup
    hp_shaderc
    hp_vulkan_loader
    # Order matters for static archives: the linker resolves left-to-right and nvrhi_vk needs symbols (utils::,
    # common::) defined in libnvrhi.a, so the dependent library must come first. Listed the other way round, those
    # members are never pulled in and the final executable fails with "undefined reference to nvrhi::utils::...".
    nvrhi_vk
    nvrhi
    Vulkan::Headers)

add_library(Hollowpoint::Graphics ALIAS HollowpointGraphics)
