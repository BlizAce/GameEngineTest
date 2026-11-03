#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

// ---------------------------------------------------------------------------
// EditorStyle — the editor's look: palette, density and fonts.
//
// Why this is a module rather than a few calls inside Start(): ImGui's default style is a *debug* style — a 13 px
// un-antialiased bitmap font, saturated blue title bars, stock padding — and every one of those defaults contributes
// to the editor reading as "a demo with panels" instead of a tool. The fix is ~40 palette entries plus metrics, and it
// has to happen before the first NewFrame, which makes it an initialisation step with its own failure modes (no font
// file on disk) that deserve names and tests rather than an inline block in EditorApplication::Start.
//
// Three entry points, split by what they need:
//   * ComputeEditorStyle(scale) is pure arithmetic — no ImGui, no filesystem — so the numbers behind the look are
//     assertable in a unit test with no display and no context.
//   * ApplyEditorTheme(scale) writes those numbers plus the palette onto the current context's style. Call after
//     CreateContext/SetCurrentContext and before the first NewFrame (again on a scale change; ImGui re-bakes glyphs on
//     demand, so that is cheap).
//   * LoadEditorFonts(outFontName) fills the current context's font atlas. It cannot fail the editor: with no readable
//     TTF anywhere it falls back to ImGui's embedded vector font — uglier, but a tool that refuses to open because
//     /usr/share/fonts moved is worse than one with plain glyphs. The fallback is reported through outFontName and
//     logged, never swallowed.
//
// Scale: metrics and font size are authored at 1.0 and multiplied by `scale`, which is a *user preference* — not
// io.DisplayFramebufferScale. HiDPI is already handled further down (the UI renderer multiplies vertex positions by
// the framebuffer scale, and ImGui rasterises glyphs at it), so folding the display scale in here would double it and
// draw a half-screen-sized UI on a Retina display.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

//! The style this module applies, as plain numbers — the assertable form of the look.
/**
 * Units: virtual (content) pixels; colours are linear-in-name-only sRGB 0..1 as ImGui stores them. Produced by
 * ComputeEditorStyle and returned by ApplyEditorTheme so callers can log/report what is in effect.
 */
struct EditorStyleInfo
{
    float Scale = 1.0f;

    //! Glyph rasterisation reference size before ImGui's own framebuffer-scale factor.
    float FontSizePixels = 0.0f;

    float WindowPaddingX = 0.0f;
    float WindowPaddingY = 0.0f;
    float FramePaddingX = 0.0f;
    float FramePaddingY = 0.0f;
    float ItemSpacingX = 0.0f;
    float ItemSpacingY = 0.0f;
    float CellPaddingX = 0.0f;
    float CellPaddingY = 0.0f;
    float ScrollbarSize = 0.0f;
    float DockSeparatorSize = 0.0f;
    float IndentSpacing = 0.0f;
};

//! Pure metrics for a UI scale: every number below is `authored * max(scale, kMinUiScale)`.
/**
 * Clamped rather than trusted because it reaches ImGui style fields that divide and offset (padding drives cursor
 * advance); a zero or negative scale produces degenerate widgets whose hit areas vanish. The clamp's floor is what
 * `ui.scale` uses as its lower bound too, so the console cannot get the editor into an unusable state.
 */
[[nodiscard]] HP_EDITOR_API EditorStyleInfo ComputeEditorStyle(float scale) noexcept;

//! Applies ComputeEditorStyle(scale) and the editor palette to the current context's style.
/**
 * Returns what was applied (for logging and `ui.info`), or a default-constructed struct when there is no current
 * context — reported rather than asserted, because this runs on a startup path that already logs its failures.
 */
[[nodiscard]] HP_EDITOR_API EditorStyleInfo ApplyEditorTheme(float scale);

//! First readable font file from the editor's candidate list, or an empty string when none exists.
/**
 * Pure filesystem query (no ImGui), which is what makes "does the editor find a font on this platform" testable. The
 * list is ordered best-first: a humanist sans at small sizes is what makes dense panels readable; the embedded bitmap
 * font ImGui defaults to is not, and that default is the single most visible reason the UI looked unprofessional.
 */
[[nodiscard]] HP_EDITOR_API std::string ResolveUiFontPath();

//! Loads the UI font (see ResolveUiFontPath) into the current context's atlas at the size ComputeEditorStyle implies.
/**
 * Call after ApplyEditorTheme and before the first NewFrame. `outFontName` receives the file name actually used, or
 * "embedded default" — it is a diagnostic string for the console and log, not a control value. Returns true when a
 * system TTF was loaded; false means the fallback is in use (still usable, worth logging once).
 */
HP_EDITOR_API bool LoadEditorFonts(std::string& outFontName);

}  // namespace Hollowpoint::Editor
