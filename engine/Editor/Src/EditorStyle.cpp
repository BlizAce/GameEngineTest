#include <algorithm>
#include <string_view>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/EditorStyle.h"

#include <imgui.h>

// ---------------------------------------------------------------------------
// See the header for the contract. Two decisions are worth restating where they live:
//
// 1. The palette's logic. A docked editor shows a dozen panels at once; ImGui's stock look gives each of them a
//    saturated blue fill, so chrome competes with the viewport for attention. Every colour here is therefore a neutral
//    grey inside a narrow luminance band, and the one accent (a muted slate blue) is reserved for things you can act on
//    or that are being acted on: focus, selection, drag previews, the active tab's overline. The viewport image is then
//    the brightest object on screen by construction — which is what "the chrome disappears" means in practice.
//
// 2. Why metrics go through ComputeEditorStyle instead of being assigned inline. They are the part of the look a test
//    can pin without a window (a dark palette and finite padding are checkable; whether it *looks* right is not), and
//    they have to scale as a set: scaling only the font leaves widgets their old padding, which reads as broken layout
//    rather than as big text.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

namespace
{

//! Bounds for the UI scale. Below the floor the metrics stop being usable before the text does (hit areas shrink with
//  padding), and `ui.scale` is a console command someone can mistype.
constexpr float kMinUiScale = 0.6f;
constexpr float kMaxUiScale = 3.0f;

[[nodiscard]] constexpr float Scaled(float base, float scale) noexcept
{
    return base * std::clamp(scale, kMinUiScale, kMaxUiScale);
}

//! sRGB 0..1 from 8-bit channels. A function because forty hand-written `0x2D / 255.0f` expressions is how two greys
//  that were meant to match end up three levels apart.
[[nodiscard]] constexpr ImVec4 Rgb(unsigned char red, unsigned char green, unsigned char blue,
                                   float alpha = 1.0f) noexcept
{
    return ImVec4(static_cast<float>(red) / 255.0f, static_cast<float>(green) / 255.0f,
                  static_cast<float>(blue) / 255.0f, alpha);
}

// --- Chrome: near-neutral dark grey with a hair of blue, so panels never out-shout the scene -----------------------
constexpr ImVec4 kWindowBg = Rgb(0x1B, 0x1D, 0x21);
constexpr ImVec4 kChildBg = Rgb(0x20, 0x22, 0x26, 0.0f);
constexpr ImVec4 kPopupBg = Rgb(0x2A, 0x2D, 0x33);
constexpr ImVec4 kBorder = Rgb(0x3C, 0x40, 0x47);
constexpr ImVec4 kMenuBarBg = Rgb(0x17, 0x19, 0x1C);

// --- Text: off-white. Pure (1,1,1) on dark grey vibrates at small sizes; UE uses an off-white for the same reason ----
constexpr ImVec4 kText = Rgb(0xE6, 0xE7, 0xE9);
constexpr ImVec4 kTextDisabled = Rgb(0x8A, 0x8F, 0x98);

// --- Widgets: input frames and buttons sit ~12 luminance points above their background; hover/active step again -----
constexpr ImVec4 kFrameBg = Rgb(0x2B, 0x2E, 0x34);
constexpr ImVec4 kFrameBgHovered = Rgb(0x3A, 0x41, 0x52);
constexpr ImVec4 kFrameBgActive = Rgb(0x46, 0x50, 0x66);

// --- Headers: flat and dark rather than blue-filled (see note 1) --------------------------------------------------
constexpr ImVec4 kHeader = Rgb(0x2F, 0x32, 0x38);
constexpr ImVec4 kHeaderHovered = Rgb(0x3E, 0x46, 0x57);
constexpr ImVec4 kHeaderActive = Rgb(0x4A, 0x59, 0x74);

// --- The accent, reserved for selection and interaction feedback --------------------------------------------------
constexpr ImVec4 kAccent = Rgb(0x5B, 0x8E, 0xD4);
constexpr ImVec4 kAccentDim = Rgb(0x3D, 0x6C, 0xA8);

}  // namespace

EditorStyleInfo ComputeEditorStyle(float scale) noexcept
{
    EditorStyleInfo info;
    info.Scale = std::clamp(scale, kMinUiScale, kMaxUiScale);

    // 15 rather than ImGui's default 13: that default is tuned for the embedded bitmap font, and at 13 a dense
    // inspector is genuinely hard to read on a modern display. With a real TTF loaded this is where UE-family editors
    // land at 100% scaling.
    info.FontSizePixels = Scaled(15.0f, scale);

    // Metrics relative to ImGui's own defaults (WindowPadding 8x8, FramePadding 4x3, ItemSpacing 8x4, CellPadding 4x2,
    // ScrollbarSize 14, IndentSpacing 21 — read from ImGuiStyle's constructor, not remembered): widget frames and spacing
    // are slightly *larger*, because they are sized for a 15 px font where ImGui's defaults serve its 13 px one, and
    // cramped frames around taller text is the failure mode being avoided. Window padding, scrollbar and indent are
    // slightly tighter, which is what keeps a docked layout reading as rows rather than as breathing room. The two
    // directions are deliberate and independent; do not "unify" them into one scale factor against the defaults.
    info.WindowPaddingX = Scaled(8.0f, scale);
    info.WindowPaddingY = Scaled(7.0f, scale);
    info.FramePaddingX = Scaled(7.0f, scale);
    info.FramePaddingY = Scaled(4.0f, scale);
    info.ItemSpacingX = Scaled(8.0f, scale);
    info.ItemSpacingY = Scaled(5.0f, scale);
    info.CellPaddingX = Scaled(5.0f, scale);
    info.CellPaddingY = Scaled(3.0f, scale);
    info.ScrollbarSize = Scaled(13.0f, scale);
    info.DockSeparatorSize = Scaled(4.0f, scale);
    info.IndentSpacing = Scaled(18.0f, scale);
    return info;
}

EditorStyleInfo ApplyEditorTheme(float scale)
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        HP_LOG_ERROR("EDITOR", "ApplyEditorTheme called with no ImGui context; the editor keeps ImGui's default style");
        return EditorStyleInfo{};
    }

    const EditorStyleInfo info = ComputeEditorStyle(scale);
    ImGuiStyle& style = ImGui::GetStyle();

    // FontSizeBase is the pre-scale reference size. It is set here — before LoadEditorFonts — because ImGui re-derives
    // it from the atlas's first font on frame one if it is still <= 0, and letting that happen would silently undo the
    // size chosen above. FontScaleMain stays 1: HiDPI is handled by ImGui's own framebuffer-scale term (see header).
    style.FontSizeBase = info.FontSizePixels;
    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = 1.0f;

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.5f;
    style.WindowPadding = ImVec2(info.WindowPaddingX, info.WindowPaddingY);
    // Square panels: rounded corners on docked panels leave visible gaps at every seam, which reads as a rendering
    // defect rather than as a design choice.
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(260.0f, 140.0f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowBorderHoverPadding = std::max(4.0f, 5.0f * info.Scale);
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(info.FramePaddingX, info.FramePaddingY);
    style.FrameRounding = Scaled(2.0f, scale);
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(info.ItemSpacingX, info.ItemSpacingY);
    style.ItemInnerSpacing = ImVec2(Scaled(5.0f, scale), Scaled(4.0f, scale));
    style.CellPadding = ImVec2(info.CellPaddingX, info.CellPaddingY);
    style.IndentSpacing = info.IndentSpacing;
    style.ScrollbarSize = info.ScrollbarSize;
    style.ScrollbarRounding = 0.0f;
    style.ScrollbarPadding = Scaled(2.0f, scale);
    style.GrabMinSize = Scaled(10.0f, scale);
    style.GrabRounding = Scaled(2.0f, scale);
    style.ImageRounding = 0.0f;
    style.ImageBorderSize = 0.0f;
    style.TabRounding = Scaled(2.0f, scale);
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.DockingSeparatorSize = info.DockSeparatorSize;
    style.SeparatorSize = std::max(1.0f, Scaled(1.0f, scale));  // ImGui requires >= 1
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;

    style.Colors[ImGuiCol_Text] = kText;
    style.Colors[ImGuiCol_TextDisabled] = kTextDisabled;
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.32f);
    style.Colors[ImGuiCol_TextLink] = kAccent;
    style.Colors[ImGuiCol_InputTextCursor] = kText;
    style.Colors[ImGuiCol_WindowBg] = kWindowBg;
    style.Colors[ImGuiCol_ChildBg] = kChildBg;
    style.Colors[ImGuiCol_PopupBg] = kPopupBg;
    style.Colors[ImGuiCol_Border] = kBorder;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_MenuBarBg] = kMenuBarBg;
    style.Colors[ImGuiCol_FrameBg] = kFrameBg;
    style.Colors[ImGuiCol_FrameBgHovered] = kFrameBgHovered;
    style.Colors[ImGuiCol_FrameBgActive] = kFrameBgActive;
    style.Colors[ImGuiCol_TitleBg] = Rgb(0x15, 0x17, 0x1A);
    style.Colors[ImGuiCol_TitleBgActive] = Rgb(0x2A, 0x2D, 0x33);
    style.Colors[ImGuiCol_TitleBgCollapsed] = Rgb(0x15, 0x17, 0x1A);
    style.Colors[ImGuiCol_ScrollbarBg] = Rgb(0x18, 0x1A, 0x1D);
    style.Colors[ImGuiCol_ScrollbarGrab] = Rgb(0x42, 0x46, 0x4E);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(0x54, 0x5B, 0x68);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = kAccentDim;
    style.Colors[ImGuiCol_CheckMark] = kAccent;
    style.Colors[ImGuiCol_CheckboxSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f);
    style.Colors[ImGuiCol_SliderGrab] = kAccentDim;
    style.Colors[ImGuiCol_SliderGrabActive] = kAccent;
    style.Colors[ImGuiCol_Button] = Rgb(0x36, 0x3A, 0x42);
    style.Colors[ImGuiCol_ButtonHovered] = Rgb(0x45, 0x4E, 0x60);
    style.Colors[ImGuiCol_ButtonActive] = kAccentDim;
    style.Colors[ImGuiCol_Header] = kHeader;
    style.Colors[ImGuiCol_HeaderHovered] = kHeaderHovered;
    style.Colors[ImGuiCol_HeaderActive] = kHeaderActive;
    style.Colors[ImGuiCol_Separator] = kBorder;
    style.Colors[ImGuiCol_SeparatorHovered] = kAccentDim;
    style.Colors[ImGuiCol_SeparatorActive] = kAccent;
    style.Colors[ImGuiCol_ResizeGrip] = Rgb(0x2E, 0x31, 0x37);
    style.Colors[ImGuiCol_ResizeGripHovered] = kAccentDim;
    style.Colors[ImGuiCol_ResizeGripActive] = kAccent;
    style.Colors[ImGuiCol_TabHovered] = kHeaderHovered;
    style.Colors[ImGuiCol_Tab] = Rgb(0x24, 0x27, 0x2C);
    style.Colors[ImGuiCol_TabSelected] = Rgb(0x33, 0x37, 0x3E);
    style.Colors[ImGuiCol_TabSelectedOverline] = kAccent;
    style.Colors[ImGuiCol_TabDimmed] = Rgb(0x20, 0x22, 0x26);
    style.Colors[ImGuiCol_TabDimmedSelected] = Rgb(0x2C, 0x2F, 0x35);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = Rgb(0x4A, 0x51, 0x5C);
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f);
    style.Colors[ImGuiCol_DockingEmptyBg] = kWindowBg;
    style.Colors[ImGuiCol_TableHeaderBg] = Rgb(0x23, 0x26, 0x2A);
    style.Colors[ImGuiCol_TableBorderStrong] = kBorder;
    style.Colors[ImGuiCol_TableBorderLight] = Rgb(0x2E, 0x31, 0x37);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    style.Colors[ImGuiCol_TreeLines] = kBorder;
    style.Colors[ImGuiCol_PlotLines] = Rgb(0xA8, 0xB2, 0xC4);
    style.Colors[ImGuiCol_PlotHistogram] = kAccentDim;

    return info;
}

std::string ResolveUiFontPath()
{
    // Best-first. Every entry is a *file path*, because ImGui's TTF loader wants one and a fontconfig family lookup
    // would add a dependency to get nothing extra. The order is "the humanist sans this size range was designed for",
    // then the Linux distributions' equivalents, then Windows and macOS.
    constexpr std::string_view kCandidates[] = {
        "/usr/share/fonts/abattis-cantarell-fonts/Cantarell-Regular.otf",
        "/usr/share/fonts/abattis-cantarell-vf-fonts/Cantarell-VF.otf",
        "/usr/share/fonts/google-droid-sans-fonts/DroidSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    };

    for (const std::string_view candidate : kCandidates)
    {
        if (Core::FileExists(candidate))
        {
            return std::string(candidate);
        }
    }
    return {};
}

bool LoadEditorFonts(std::string& outFontName)
{
    outFontName.clear();

    if (ImGui::GetCurrentContext() == nullptr)
    {
        HP_LOG_ERROR("EDITOR", "LoadEditorFonts called with no ImGui context; no font was loaded");
        outFontName = "none (no context)";
        return false;
    }

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const float sizePixels = std::max(8.0f, ImGui::GetStyle().FontSizeBase);

    const std::string path = ResolveUiFontPath();
    if (!path.empty())
    {
        // SizePixels is the reference size and is deliberately *not* multiplied by the display scale: ImGui 1.93 bakes
        // glyphs at io.DisplayFramebufferScale itself, so pre-multiplying here would apply HiDPI twice — the mistake its
        // own migration notes warn about for the retired FontGlobalScale idiom.
        if (atlas->AddFontFromFileTTF(path.c_str(), sizePixels) != nullptr)
        {
            outFontName = path;
            return true;
        }

        // Exists but unreadable by stb_truetype (a corrupt file, or a flavour it rejects). Reported, then the embedded
        // fallback below still leaves the editor usable.
        HP_LOG_WARNING("EDITOR", "font \"{}\" could not be rasterised; using ImGui's embedded font", path);
    }

    // The embedded *vector* font, never AddFontDefaultBitmap: the bitmap one is 13 px and pixelated at any scale other
    // than exactly 1x, and it being chosen by default is precisely what made the previous look unreadable.
    if (atlas->AddFontDefaultVector(nullptr) == nullptr)
    {
        outFontName = "none (embedded default failed)";
        HP_LOG_ERROR("EDITOR", "could not add the embedded default font; UI text will not render");
        return false;
    }

    outFontName = "ImGui embedded vector font";
    return false;
}

}  // namespace Hollowpoint::Editor
