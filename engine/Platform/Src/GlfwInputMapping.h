#pragma once

#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Platform/GlfwInclude.h"
#include "Hollowpoint/Platform/InputEvent.h"

namespace Hollowpoint::Platform::Detail
{

// ---------------------------------------------------------------------------
// GLFW -> engine input translation. Internal to the Platform module.
//
// Separated from Window.cpp because it is the part most likely to be wrong (a hand-maintained table of
// ~120 entries) and most likely to need extending, and because a test can exercise it directly without a
// window: the mapping is pure, so its correctness does not depend on having a display server.
// ---------------------------------------------------------------------------

//! Translates a GLFW key code (including the special GLFW_KEY_UNKNOWN sentinel). Unmapped codes yield
//! KeyCode::Unknown rather than an arbitrary key — inventing input is worse than dropping it.
[[nodiscard]] KeyCode TranslateKey(int glfwKey) noexcept;

//! Translates GLFW's press/repeat/release action.
[[nodiscard]] InputAction TranslateAction(int glfwAction) noexcept;

[[nodiscard]] KeyModifier TranslateModifiers(int glfwMods) noexcept;

[[nodiscard]] MouseButton TranslateMouseButton(int glfwButton) noexcept;

//! Decodes one UTF-8 sequence into a code point, as delivered by the file-drop and (on some backends)
//! text paths. Invalid sequences produce U+FFFD rather than being dropped: silently eating a character is
//! invisible, a replacement character is not.
[[nodiscard]] char32_t DecodeUtf8CodePoint(const char* text) noexcept;

}  // namespace Hollowpoint::Platform::Detail
