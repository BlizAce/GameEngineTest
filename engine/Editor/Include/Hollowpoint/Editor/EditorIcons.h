#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/RefCounted.h"
#include "Hollowpoint/Core/Types.h"

// ---------------------------------------------------------------------------
// EditorIcons — the editor's glyph atlas: where each icon lives, and how it gets onto screen.
//
// Why a module rather than ImGui::Image calls with hand-written UVs scattered through the panels: an icon's cell is a
// property of the *asset*, and the asset is authored by scripts/make_editor_icons.py. Duplicating those coordinates in
// widget code means any change to the atlas silently mislabels every button — the worst kind of UI bug, because it looks
// like a working interface. So the layout table lives here once, and TestEditorIcons asserts it against the manifest the
// authoring script emits, which turns "someone inserted an icon in one file and not the other" into a test failure.
//
// Three layers, split by what they need:
//   * IconAtlasLayout is a compile-time table — no filesystem, no GPU, no ImGui — so cell arithmetic is assertable in a
//     unit test with nothing but a CPU. This is also the form used before the atlas has loaded (see IsLoaded).
//   * DecodeIconAtlasPng + BleedAtlasAlpha are pure pixel work on bytes: PNG in, RGBA8 out. Testable without a device,
//     which matters because both have failure modes that are invisible until something renders wrong (a flipped image,
//     or dark fringes around every glyph).
//   * EditorIconSet owns the GPU texture and its ImGui id. It is the only part that needs a live renderer, and it
//     degrades honestly: no file or no device means no icons drawn, reported once in the log — never a wrong glyph.
//
// Alpha bleeding (see BleedAtlasAlpha) is not optional polish. The UI sampler is linear, so drawing a 16 px glyph from
// a 32 px cell samples across the cell's border into fully-transparent texels whose RGB channels are black; the result
// is a visible dark halo on every icon. Pushing each transparent texel's colour to its nearest opaque neighbour removes
// the fringe while leaving alpha untouched, and it only has to happen once at load.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{
class GraphicsDevice;
class Texture;
}  // namespace Hollowpoint::Graphics

namespace Hollowpoint::Editor
{

class UiRenderer;

using Core::Size;
using Core::UInt8;
using Core::UInt32;
using Core::UInt64;

//! One glyph's cell as texture coordinates: top-left (U0,V0) and bottom-right (U1,V1), 0..1.
/**
 * UVs rather than a pixel rect because that is what a widget hands ImGui, and because the atlas extent belongs to the
 * asset: a caller that multiplied a cell rect by an atlas size it assumed would be wrong the moment the atlas is
 * re-rendered at 2x. All coordinates are in ImGui's convention (V=0 at the top), which is also how the atlas PNG is
 * stored — no flip anywhere in this path (see DecodeIconAtlasPng for why that is not a coincidence).
 */
struct IconUvRect
{
    float U0 = 0.0f;
    float V0 = 0.0f;
    float U1 = 0.0f;
    float V1 = 0.0f;

    [[nodiscard]] float Width() const noexcept
    {
        return U1 - U0;
    }

    [[nodiscard]] float Height() const noexcept
    {
        return V1 - V0;
    }
};

//! The atlas' geometry and its name-to-cell table, as compiled into the editor.
/**
 * Deliberately value-like and cheap to copy so a test can build one and check arithmetic without touching global state;
 * Default() is the shared instance the editor uses. Names are stable identifiers (snake_case, matching the manifest),
 * not display text — a label belongs next to the icon in the widget, where it can be localised.
 */
class HP_EDITOR_API IconAtlasLayout
{
public:
    //! The layout produced by scripts/make_editor_icons.py; the only one the editor ships.
    [[nodiscard]] static const IconAtlasLayout& Default() noexcept;

    [[nodiscard]] UInt32 GetAtlasWidth() const noexcept
    {
        return m_AtlasWidth;
    }

    [[nodiscard]] UInt32 GetAtlasHeight() const noexcept
    {
        return m_AtlasHeight;
    }

    //! Cell edge in texels. Every cell is square by construction, so one number suffices.
    [[nodiscard]] UInt32 GetCellSize() const noexcept
    {
        return m_CellSize;
    }

    [[nodiscard]] Size GetIconCount() const noexcept
    {
        return m_Count;
    }

    //! Name of the icon at `index`, or an empty view when out of range. Used by diagnostics and by the test that walks
    //  the whole table (an out-of-range index is a caller bug, reported rather than asserted: it arrives from loops).
    [[nodiscard]] std::string_view GetName(Size index) const noexcept;

    //! UVs for `name`, false when the name is unknown.
    /**
     * False is the honest answer and also the useful one: a widget that gets false draws its text label instead of an
     * icon, so a missing glyph degrades to "no picture" rather than to "someone else's picture". Unknown names are not
     * logged here (a panel asks for icons every frame); see EditorIconSet::Load, which reports table/asset mismatches
     * once.
     */
    [[nodiscard]] bool Find(std::string_view name, IconUvRect& outRect) const noexcept;

    //! Cell rectangle in texels — the integer form of Find, for tests and for asset-side tooling.
    [[nodiscard]] bool FindCell(std::string_view name, UInt32& outX, UInt32& outY) const noexcept;

    //! Index of `name` in the table, or std::string_view::npos. Exposed so a test can assert ordering against the
    //  manifest without duplicating the search.
    [[nodiscard]] Size IndexOf(std::string_view name) const noexcept;

    IconAtlasLayout() = default;

    //! Builder for tests and tooling: define the geometry, then append names in cell order (row-major).
    void Build(UInt32 atlasWidth, UInt32 atlasHeight, UInt32 cellSize) noexcept
    {
        m_AtlasWidth = atlasWidth;
        m_AtlasHeight = atlasHeight;
        m_CellSize = cellSize;
        m_Names.clear();
        m_Count = 0;
    }

    void AppendName(std::string_view name)
    {
        m_Names.emplace_back(name);
        m_Count = m_Names.size();
    }

private:
    [[nodiscard]] Size ColumnOf(Size index) const noexcept
    {
        return m_CellSize > 0 ? index % (m_AtlasWidth / m_CellSize) : 0;
    }

    [[nodiscard]] Size RowOf(Size index) const noexcept
    {
        const Size columns = m_CellSize > 0 ? m_AtlasWidth / m_CellSize : 1;
        return columns > 0 ? index / columns : 0;
    }

    UInt32 m_AtlasWidth = 0;
    UInt32 m_AtlasHeight = 0;
    UInt32 m_CellSize = 0;
    Size m_Count = 0;
    std::vector<std::string> m_Names;
};

//! An RGBA8 image decoded from an icon atlas PNG: tightly packed, top row first.
struct DecodedIconAtlas
{
    UInt32 Width = 0;
    UInt32 Height = 0;
    std::vector<UInt8> Rgba;  //!< Width * Height * 4, no padding.

    [[nodiscard]] bool IsValid() const noexcept
    {
        return Width > 0 && Height > 0 && Rgba.size() == static_cast<size_t>(Width) * Height * 4u;
    }
};

//! Reads a PNG as RGBA8 with the first byte being its top-left pixel.
/**
 * Returns false and fills `outError` when the file cannot be read or is not a decodable image — reported rather than
 * thrown, because this runs on the editor's startup path where "no icons" is a degraded but working state.
 *
 * The flip matters more than it sounds: stb_image yields bottom-up rows by default (GL convention) while ImGui's UV
 * space and this module's layout table are top-down. Setting STB_IMAGE_VERTICAL_FLIP is the one place that reconciles
 * them; leaving it out produces an atlas whose every glyph is upside down, which at 16 px reads as "the icons are just
 * bad" rather than "the image is inverted".
 */
[[nodiscard]] HP_EDITOR_API bool DecodeIconAtlasPng(const std::string& path, DecodedIconAtlas& outAtlas,
                                                    std::string* outError = nullptr);

//! Pushes every fully-transparent texel's colour to the nearest opaque texel, in place, leaving alpha at zero.
/**
 * One pass per side with a shrinking distance is enough for an icon atlas: transparent texels adjacent to opaque ones
 * are fixed on pass 1, and interior transparent regions (the middle of a ring) only need *a* plausible colour, not the
 * exact one — they are never sampled alone. Without this, linear filtering blends glyph edges toward black texels and
 * every icon gets a dark outline against the panel background.
 */
void BleedAtlasAlpha(DecodedIconAtlas& atlas) noexcept;

//! Icon edge for a UI scale factor, clamped so it never vanishes or flips.
/**
 * Icons must track the font size they sit beside — an icon smaller than its row's text reads as detached from its
 * label — so this is 16 px at 1x and scales with the slider. `minPixels` guards against a hostile scale producing 0
 * (an invisible glyph) or a negative size (a mirrored draw); it defaults low because callers legitimately ask for
 * small icons in dense rows, and clamping at 16 would silently defeat scaling altogether. A non-positive scale is not
 * a size request at all, so it returns the unscaled default rather than something near zero.
 */
[[nodiscard]] HP_EDITOR_API float IconSizeForScale(float uiScale, float minPixels = 4.0f) noexcept;

//! The loaded atlas: a GPU texture plus the ImGui id widgets draw with.
/**
 * The only part of this module that needs a renderer, kept separate so the layout and pixel work above stay testable
 * without a device. Load is called once after the UI renderer exists; failure leaves the set unloaded and the editor
 * drawing text labels instead of icons — reported once in the log rather than retried per frame or turned into a
 * startup error, because an editor with no icons is still an editor.
 */
class HP_EDITOR_API EditorIconSet
{
public:
    //! Defined in the .cpp because m_Texture's destructor needs Graphics::Texture complete; the header forward-declares it.
    ~EditorIconSet();

    EditorIconSet() = default;
    EditorIconSet(const EditorIconSet&) = delete;
    EditorIconSet& operator=(const EditorIconSet&) = delete;

    //! Reads `<projectRoot>/assets/editor/icons/hp_editor_icons.png`, uploads it, and registers it with `ui`.
    /**
     * `fallbackPaths` are tried after the project-relative one, in order — the install tree and the source tree are
     * different directories, and an editor that only finds its icons when launched from the repo root breaks exactly
     * the way a .desktop shortcut does (wrong working directory, silent degradation).
     *
     * Returns true when the atlas is loaded. `outError` receives the reason otherwise; the log gets one line either way,
     * including which path was used, because "which of these four candidates won" is the first question when icons are
     * missing.
     */
    bool Load(Graphics::GraphicsDevice& device, UiRenderer& ui, const std::string& projectRoot,
              const std::vector<std::string>& fallbackPaths = {}, std::string* outError = nullptr);

    //! Releases the texture registration. Safe to call twice; called from the application's shutdown path.
    void Unload(UiRenderer& ui);

    [[nodiscard]] bool IsLoaded() const noexcept
    {
        return m_TextureId != 0;
    }

    //! The ImGui texture id to draw with, or 0 when unloaded. Widgets must check this before drawing an icon.
    [[nodiscard]] UInt64 GetTextureId() const noexcept
    {
        return m_TextureId;
    }

    [[nodiscard]] const IconAtlasLayout& GetLayout() const noexcept
    {
        return m_Layout;
    }

    //! Path the atlas was read from, empty when unloaded. Reported by `ui.info`.
    [[nodiscard]] const std::string& GetSourcePath() const noexcept
    {
        return m_SourcePath;
    }

    //! UVs for `name` when loaded, false otherwise — so a widget's icon lookup is one call that already accounts for
    //  the atlas not being there.
    [[nodiscard]] bool Find(std::string_view name, IconUvRect& outRect) const noexcept
    {
        return m_TextureId != 0 && m_Layout.Find(name, outRect);
    }

private:
    Core::Ref<Graphics::Texture> m_Texture;  //!< Owned; keeps the GPU resource alive while widgets can draw with it.
    UInt64 m_TextureId = 0;
    std::string m_SourcePath;
    IconAtlasLayout m_Layout = IconAtlasLayout::Default();
};

}  // namespace Hollowpoint::Editor
