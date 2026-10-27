#pragma once

#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Platform/InputEvent.h"

// ---------------------------------------------------------------------------
// EditorInput — feeds Dear ImGui from the engine's own input queue.
//
// Why not imgui_impl_glfw: GLFW's callbacks never fire for events delivered through Platform::Window::InjectEvent,
// which is the engine's documented seam for agent-driven operation (the reason a scripted "select entity, type a name"
// session must work at all). A GLFW-fed ImGui would be deaf to exactly the automation this editor is required to
// support — and it would be a second input path that can disagree with the first about ordering. So the editor maps
// InputEvents onto ImGui's queued-event API (io.AddKeyEvent & friends), which is backend-agnostic by design.
//
// The mapping is a translation table with three non-obvious parts, each covered by a test:
//   * Key codes. Engine KeyCode → ImGuiKey, one switch. Unmapped keys report "no key" rather than guessing — an
//     arbitrary mapping would make Ctrl+<something> mean a different editor action than the author pressed, which is
//     discovered at the worst possible moment (a delete).
//   * Text. The engine delivers decoded code points as Character events precisely so non-US layouts work; those go to
//     io.AddInputCharacter untouched. Translating keys into characters instead would break every keyboard that is not
//     US and silently mangle typed entity names.
//   * Focus. ImGui needs to know when the window loses focus (to stop routing text), and the engine reports it as a
//     Focus event with a bool — an event type with no ImGui equivalent in the per-key API, so it is applied as state.
//
// Mouse position arrives in content coordinates (Platform's contract: DPI already divided out). ImGui wants the same
// space it is told about via io.DisplaySize, and the editor sets DisplaySize from the window's *content* size for that
// reason — mixing framebuffer pixels into one and content units into the other puts every widget a scale factor away
// from the cursor, on HiDPI displays only.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

using Core::Index;

//! Applies engine input events to the current ImGui context.
/**
 * Call once per frame, after ImGui::NewFrame's predecessor (the queue is drained by the runtime loop before any host
 * drawing) and before ImGui::NewFrame. Events are applied in queue order, which is what makes a recorded event stream
 * reproduce a session: ImGui processes queued events in the order they were added, so re-ordering here would break
 * "press Ctrl, then A" versus "press A, then Ctrl".
 *
 * Returns how many events were translated into at least one ImGui input call. Not for logging — for tests and for the
 * `editor.input_stats` console command, which is how "the UI stopped receiving input" gets distinguished from "the UI
 * received it and ignored it" without a debugger attached.
 */
[[nodiscard]] HP_EDITOR_API Index FeedImGuiInput(const std::vector<Platform::InputEvent>& events);

//! Applies window-level state ImGui tracks outside the event queue: display size, delta time and focus.
/**
 * DisplaySize is in content units (see the header note). `hasFocus` comes from the last Focus event or the window's own
 * state; a stale false would leave ImGui refusing text entry after an alt-tab, which is one of those bugs users learn
 * to work around rather than report.
 */
void HP_EDITOR_API FeedImGuiFrameState(float contentWidth, float contentHeight, float deltaSeconds, bool hasFocus);

//! The ImGui key equivalent of an engine key code, or -1 when there is no mapping. Exposed for tests and for the
//! binding UI; callers that just want input fed should use FeedImGuiInput.
[[nodiscard]] HP_EDITOR_API int ImGuiKeyFor(Platform::KeyCode key) noexcept;

//! True when `key` maps to an ImGui key, i.e. ImGui will see it at all. Used by tests to assert coverage of the ranges
//! the engine enum defines (letters, digits, function keys, keypad) rather than spot-checking a few names.
[[nodiscard]] HP_EDITOR_API bool HasImGuiKeyMapping(Platform::KeyCode key) noexcept;

}  // namespace Hollowpoint::Editor
