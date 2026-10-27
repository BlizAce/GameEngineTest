# Hollowpoint Editor: the authoring tool built on the runtime, not beside it.
#
# Why a module rather than an application directory: everything here is engine code with the same contract as the rest
# (tested, no placeholders, honest APIs), and the alternative — putting it in apps/ — is how editors rot into being one
# untested blob that duplicates the frame loop. The layering rule this module exists to enforce: Runtime knows nothing
# about the editor (it exposes IApplicationHost), while the editor may use everything below it. Nothing under engine/
# links this target.
#
# What is here, and why each piece is separate enough to test on its own:
#   * Picking — rays and geometry in, an entity out. No window, no GPU, no ImGui: click-to-select is where an
#     unnormalised ray or a missing perspective divide reads as "picks are a bit off" for a week.
#   * EntityName + registration — authoring metadata that must not enter the simulation's hot paths.
#   * EditorInput — engine InputEvents in, ImGui IO out. The seam that keeps injected events (the agent-driven path)
#     reaching the UI, which GLFW-fed backends cannot do.
#   * UiRenderer — draws ImGui's draw data through the engine's own Pipeline/Buffer/Texture wrappers, because the stock
#     Vulkan backend would fight NVRHI over one image's layout.
#   * EditorApplication — hosts RuntimeApplication, owns the viewport target and the docked panels.
#
# Not here yet, and tracked in notes/EDITOR.md rather than stubbed: gizmos with undo, prefabs/drag-drop into the
# viewport, an asset database behind the content browser (it lists files; it does not import them), multi-select
# transforms and material editing beyond what reflection exposes generically.

add_library(HollowpointEditor STATIC)
hp_set_engine_target(HollowpointEditor)

target_include_directories(HollowpointEditor PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointEditor PRIVATE
    Include/Hollowpoint/Editor/ContentBrowserModel.h
    Include/Hollowpoint/Editor/EditorApplication.h
    Include/Hollowpoint/Editor/EditorPanels.h
    Include/Hollowpoint/Editor/EditorConsole.h
    Include/Hollowpoint/Editor/EditorInput.h
    Include/Hollowpoint/Editor/EditorSelection.h
    Include/Hollowpoint/Editor/EntityName.h
    Include/Hollowpoint/Editor/Picking.h
    Include/Hollowpoint/Editor/PropertyView.h
    Include/Hollowpoint/Editor/UiRenderer.h
    Src/ContentBrowserModel.cpp
    Src/EditorApplication.cpp
    Src/EditorPanels.cpp
    Src/EditorConsole.cpp
    Src/EditorInput.cpp
    Src/EditorSelection.cpp
    Src/Picking.cpp
    Src/PropertyView.cpp
    Src/RegisterEditorTypes.cpp
    Src/UiRenderer.cpp)

target_link_libraries(HollowpointEditor PUBLIC
    Hollowpoint::Core
    Hollowpoint::Math
    Hollowpoint::ECS
    Hollowpoint::Scene
    Hollowpoint::Reflection
    Hollowpoint::Serialization
    Hollowpoint::Platform
    Hollowpoint::Graphics
    Hollowpoint::Renderer
    Hollowpoint::Game
    Hollowpoint::Runtime
    Hollowpoint::CompilerSetup)

# ImGui is fetched only with the editor and Vulkan both on (see third_party/CMakeLists.txt), and this module's UI code
# assumes it — so the target is a hard dependency here rather than an optional one with #ifdefs scattered through the
# panels. A build without ImGui does not get an editor; that is stated at configure time instead of failing to link.
target_link_libraries(HollowpointEditor PUBLIC Hollowpoint::ImGui)

if(TARGET HollowpointScripting)
    target_link_libraries(HollowpointEditor PUBLIC Hollowpoint::Scripting)
endif()

add_library(Hollowpoint::Editor ALIAS HollowpointEditor)

# ---------------------------------------------------------------------------
# The editor executable.
#
# Argument parsing and exit codes only, exactly like hollowpoint-runtime: behaviour lives in the library and is tested
# there, so this target's job is to prove the library links and starts. Its existence is also the module graph's smoke
# test — a cycle or missing link dependency surfaces here rather than as an undefined symbol later.
# ---------------------------------------------------------------------------
if(HP_BUILD_EDITOR)
    add_executable(HollowpointEditorApp Src/Main.cpp)
    hp_set_engine_target(HollowpointEditorApp)

    target_link_libraries(HollowpointEditorApp PRIVATE Hollowpoint::Editor)

    set_target_properties(HollowpointEditorApp PROPERTIES OUTPUT_NAME "hollowpoint-editor" FOLDER "apps")
endif()
