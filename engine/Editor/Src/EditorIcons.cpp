#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

#include "Hollowpoint/Core/FileSystem.h"
#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/EditorIcons.h"
#include "Hollowpoint/Editor/UiRenderer.h"
#include "Hollowpoint/Graphics/GraphicsDevice.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"
#include "Hollowpoint/Graphics/Texture.h"

// stb_image is the decoder the texture importer uses elsewhere (Runtime/Src/Screenshot.cpp pairs it with
// stb_image_write), so icons add no new third-party dependency — only the implementation define, in this one TU.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// ---------------------------------------------------------------------------
// See the header for the contract. The decisions worth restating where they live:
//
// 1. Why the layout table is generated (EditorIconTable.inl) rather than written here. The atlas is authored by
//    scripts/make_editor_icons.py; a hand-maintained copy of its cell order is a second source of truth, and the first
//    icon someone inserts changes every button below it in one file but not the other — producing an interface whose
//    icons are all subtly wrong, which users describe as "confusing" rather than as a bug. The script emits both sides,
//    and TestEditorIcons checks this table against the manifest AND against ink actually present in the PNG.
//
// 2. Why loading degrades instead of failing. A missing atlas is a packaging problem (an install tree without
//    assets/, a wrong working directory), not a programmer error, and an editor that draws text labels instead of icons
//    is still fully usable. So Load returns false with a reason, logs once, and the widgets fall back to labels — they
//    never draw a *different* icon, which would be worse than none.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

namespace
{

constexpr const char* kLogChannel = "EDITOR";

//! The atlas' geometry, mirrored from the authoring script and asserted against it by TestEditorIcons.
constexpr UInt32 kAtlasWidth = 512;
constexpr UInt32 kAtlasHeight = 512;
constexpr UInt32 kAtlasCell = 32;

//! Names in cell order, generated. Included once, here, so the table has exactly one home in the binary.
const char* const kIconNames[] = {
#include "EditorIconTable.inl"
};

[[nodiscard]] const std::vector<std::string_view>& IconNameViews()
{
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> views;
        views.reserve(std::size(kIconNames));
        for (const char* name : kIconNames)
        {
            views.emplace_back(name);
        }
        return views;
    }();
    return names;
}

}  // namespace

// ---------------------------------------------------------------------------
// IconAtlasLayout
// ---------------------------------------------------------------------------

const IconAtlasLayout& IconAtlasLayout::Default() noexcept
{
    // Built once from the generated table. Lazy rather than a static initialiser at namespace scope because it holds
    // std::string: a translation-unit initialiser would run before main and make startup order matter for no reason.
    static const IconAtlasLayout layout = [] {
        IconAtlasLayout built;
        built.Build(kAtlasWidth, kAtlasHeight, kAtlasCell);
        for (const std::string_view name : IconNameViews())
        {
            built.AppendName(name);
        }
        return built;
    }();
    return layout;
}

std::string_view IconAtlasLayout::GetName(Size index) const noexcept
{
    if (index >= m_Names.size())
    {
        return {};
    }
    return m_Names[index];
}

Size IconAtlasLayout::IndexOf(std::string_view name) const noexcept
{
    for (Size i = 0; i < static_cast<Size>(m_Names.size()); ++i)
    {
        if (m_Names[i] == name)
        {
            return i;
        }
    }
    return std::string_view::npos;
}

bool IconAtlasLayout::FindCell(std::string_view name, UInt32& outX, UInt32& outY) const noexcept
{
    if (m_CellSize == 0)
    {
        return false;
    }

    const Size index = IndexOf(name);
    if (index == std::string_view::npos)
    {
        return false;
    }

    outX = static_cast<UInt32>(ColumnOf(index) * m_CellSize);
    outY = static_cast<UInt32>(RowOf(index) * m_CellSize);
    return true;
}

bool IconAtlasLayout::Find(std::string_view name, IconUvRect& outRect) const noexcept
{
    if (m_AtlasWidth == 0 || m_AtlasHeight == 0)
    {
        return false;
    }

    UInt32 x = 0;
    UInt32 y = 0;
    if (!FindCell(name, x, y))
    {
        return false;
    }

    outRect.U0 = static_cast<float>(x) / static_cast<float>(m_AtlasWidth);
    outRect.V0 = static_cast<float>(y) / static_cast<float>(m_AtlasHeight);
    outRect.U1 = static_cast<float>(x + m_CellSize) / static_cast<float>(m_AtlasWidth);
    outRect.V1 = static_cast<float>(y + m_CellSize) / static_cast<float>(m_AtlasHeight);
    return true;
}

// ---------------------------------------------------------------------------
// Decoding and preprocessing
// ---------------------------------------------------------------------------

bool DecodeIconAtlasPng(const std::string& path, DecodedIconAtlas& outAtlas, std::string* outError)
{
    outAtlas = DecodedIconAtlas{};

    // stb_image returns rows in file order (top row first), which is what ImGui's UV space and this module's layout
    // table expect — so NO flip here. Calling stbi_set_flip_vertically_on_load(1) on the theory that stb defaults to
    // GL's bottom-up was wrong, and it failed in a nasty way: the decoded image came back mirrored, so every cell
    // lookup read a different glyph's texels (row 0 sampled the empty row 15) and most icons drew nothing at all.
    // The per-cell ink check in TestEditorIcons catches exactly this.
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        // stbi_failure_reason() names the stage that failed (unsupported format vs truncated file vs no such file),
        // which is the difference between "the asset is missing" and "the asset is corrupt".
        const std::string reason = stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown";
        if (outError != nullptr)
        {
            *outError = "could not decode '" + path + "' (" + reason + ")";
        }
        return false;
    }

    if (width <= 0 || height <= 0)
    {
        stbi_image_free(pixels);
        if (outError != nullptr)
        {
            *outError = "'" + path + "' decoded to a degenerate size";
        }
        return false;
    }

    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    outAtlas.Width = static_cast<UInt32>(width);
    outAtlas.Height = static_cast<UInt32>(height);
    outAtlas.Rgba.assign(pixels, pixels + byteCount);
    stbi_image_free(pixels);

    return outAtlas.IsValid();
}

namespace
{

//! The three buffers alpha bleeding works between, kept together so the pass loop cannot mix them up.
/**
 * `Source` is the state at the start of the current pass and `Next` accumulates this pass's results; reading colour
 * only from Source is what makes pass N reach exactly distance N. A single in-place buffer would instead flood the
 * whole image in scan order — the classic bug in hand-written dilation, and one that looks fine on a 1-texel ring.
 */
struct DecodedAtlasBuffers
{
    int Width = 0;
    int Height = 0;
    std::vector<UInt8> Source;
    std::vector<UInt8> Next;
};

//! Copies colour from *an* opaque texel exactly `pass` away, if one exists. Returns whether it did.
/**
 * Split out so the pass loop reads as "for each transparent texel, pull colour in from this ring" and nothing else —
 * the early-exit bookkeeping that made the inline version hard to read (and easy to get wrong) lives here instead.
 *
 * Which texel on the ring wins is scan order, not distance: every candidate on a given ring is equidistant in the
 * Chebyshev metric this dilation uses, so there is no nearer one to prefer. What matters for the anti-aliasing result
 * is only that the colour came from a real glyph edge rather than from black.
 */
[[nodiscard]] bool BleedOne(DecodedAtlasBuffers& buffers, int x, int y, int pass)
{
    const size_t width = static_cast<size_t>(buffers.Width);
    const size_t here = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4u;

    for (int dy = -pass; dy <= pass; ++dy)
    {
        for (int dx = -pass; dx <= pass; ++dx)
        {
            // Only the square ring at exactly this distance — nearer texels were this texel's business on an earlier
            // pass and are known to be transparent.
            if (std::abs(dx) != pass && std::abs(dy) != pass)
            {
                continue;
            }

            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= buffers.Width || ny >= buffers.Height)
            {
                continue;
            }

            const size_t neighbour = (static_cast<size_t>(ny) * width + static_cast<size_t>(nx)) * 4u;
            if (buffers.Source[neighbour + 3] == 0)
            {
                continue;
            }

            buffers.Next[here + 0] = buffers.Source[neighbour + 0];
            buffers.Next[here + 1] = buffers.Source[neighbour + 1];
            buffers.Next[here + 2] = buffers.Source[neighbour + 2];
            // Alpha deliberately stays 0: bleeding changes what a *filtered* sample reads, never whether the texel is
            // opaque. Copying alpha here would turn every cell into a solid rectangle.
            return true;
        }
    }
    return false;
}

}  // namespace

void BleedAtlasAlpha(DecodedIconAtlas& atlas) noexcept
{
    if (!atlas.IsValid())
    {
        return;
    }

    DecodedAtlasBuffers buffers;
    buffers.Width = static_cast<int>(atlas.Width);
    buffers.Height = static_cast<int>(atlas.Height);
    buffers.Source = atlas.Rgba;
    buffers.Next = atlas.Rgba;

    // Each pass dilates colour by one texel. Glyph strokes are a few texels wide and cells are 32, so a handful of
    // passes covers any interior gap; the cap keeps a pathological all-transparent cell from looping pointlessly —
    // those texels stay black, and they are never sampled alone anyway.
    for (int pass = 1; pass <= 8; ++pass)
    {
        bool changed = false;

        for (int y = 0; y < buffers.Height; ++y)
        {
            for (int x = 0; x < buffers.Width; ++x)
            {
                const size_t here =
                    (static_cast<size_t>(y) * static_cast<size_t>(buffers.Width) + static_cast<size_t>(x)) * 4u;
                if (buffers.Source[here + 3] != 0)
                {
                    continue;  // already has colour: originally opaque, or bleeded in an earlier pass
                }
                changed = BleedOne(buffers, x, y, pass) || changed;
            }
        }

        if (!changed)
        {
            break;
        }

        atlas.Rgba = buffers.Next;
        buffers.Source = buffers.Next;
    }
}

float IconSizeForScale(float uiScale, float minPixels) noexcept
{
    // A non-positive scale is not a size request (it would mean "draw nothing"), so give the unscaled default rather
    // than clamping toward zero — and never return a negative edge, which a draw call would interpret as mirrored UVs.
    if (!(uiScale > 0.0f))
    {
        return 16.0f;
    }
    return std::max(minPixels, 16.0f * uiScale);
}

// ---------------------------------------------------------------------------
// EditorIconSet
// ---------------------------------------------------------------------------

namespace
{

//! Candidate atlas locations, in the order they should be tried.
/**
 * Project-relative first (that is where a project would override the built-in set), then relative to the working
 * directory. The working-directory candidates exist because every automated capture in this repo launches the editor
 * from wherever the shell happens to be — an asset that resolves only from one directory produces exactly the failure
 * mode that made an earlier launcher silently fall back to flat colours: a WARNING line nobody reads and a UI that
 * looks intentionally plain.
 */
[[nodiscard]] std::vector<std::string> AtlasCandidates(const std::string& projectRoot)
{
    const std::string relative = "assets/editor/icons/hp_editor_icons.png";

    std::vector<std::string> paths;
    if (!projectRoot.empty())
    {
        paths.push_back(projectRoot + "/" + relative);
    }
    paths.push_back("./" + relative);
    paths.push_back("../" + relative);
    return paths;
}

}  // namespace

EditorIconSet::~EditorIconSet() = default;

bool EditorIconSet::Load(Graphics::GraphicsDevice& device, UiRenderer& ui, const std::string& projectRoot,
                         const std::vector<std::string>& fallbackPaths, std::string* outError)
{
    std::vector<std::string> candidates = AtlasCandidates(projectRoot);
    candidates.insert(candidates.end(), fallbackPaths.begin(), fallbackPaths.end());

    std::string chosen;
    for (const std::string& candidate : candidates)
    {
        if (!Core::FileExists(candidate))
        {
            continue;
        }

        DecodedIconAtlas atlas;
        std::string decodeError;
        if (!DecodeIconAtlasPng(candidate, atlas, &decodeError))
        {
            if (outError != nullptr)
            {
                *outError = decodeError;
            }
            HP_LOG_ERROR(kLogChannel, "%s", decodeError.c_str());
            return false;  // present but corrupt: trying other paths would hide a broken asset
        }

        if (atlas.Width != m_Layout.GetAtlasWidth() || atlas.Height != m_Layout.GetAtlasHeight())
        {
            const std::string message = "icon atlas '" + candidate + "' is " + std::to_string(atlas.Width) + "x" +
                                        std::to_string(atlas.Height) + ", expected " +
                                        std::to_string(m_Layout.GetAtlasWidth()) + "x" +
                                        std::to_string(m_Layout.GetAtlasHeight());
            if (outError != nullptr)
            {
                *outError = message;
            }
            HP_LOG_ERROR(kLogChannel, "%s", message.c_str());
            return false;  // a size mismatch means every UV would be wrong: refuse rather than guess
        }

        BleedAtlasAlpha(atlas);

        Graphics::TextureDesc desc;
        desc.Name = "editor-icons";
        desc.Width = atlas.Width;
        desc.Height = atlas.Height;
        desc.Format = Graphics::GraphicsFormat::Rgba8Unorm;
        desc.IsRenderTarget = false;
        desc.IsShaderResource = true;

        Ref<Graphics::Texture> texture = Graphics::Texture::Create(device, desc);
        if (texture == nullptr)
        {
            if (outError != nullptr)
            {
                *outError = "could not create the icon atlas texture";
            }
            return false;
        }

        if (!texture->Update(device, atlas.Rgba.data(), atlas.Rgba.size(), 0))
        {
            if (outError != nullptr)
            {
                *outError = "could not upload the icon atlas (" + std::to_string(atlas.Width) + "x" +
                            std::to_string(atlas.Height) + ")";
            }
            return false;
        }

        const UInt64 textureId = ui.RegisterTexture(texture);
        if (textureId == 0)
        {
            if (outError != nullptr)
            {
                *outError = "the UI renderer refused the icon atlas";
            }
            return false;
        }

        m_Texture = std::move(texture);
        m_TextureId = textureId;
        chosen = candidate;
        break;
    }

    if (m_TextureId == 0)
    {
        const std::string message = "no icon atlas found (tried: " + [&] {
            std::string joined;
            for (const std::string& candidate : candidates)
            {
                if (!joined.empty())
                {
                    joined += ", ";
                }
                joined += candidate;
            }
            return joined;
        }() + ")";
        if (outError != nullptr)
        {
            *outError = message;
        }
        // A warning, not an error: the editor is fully usable without icons, and a red line for a cosmetic asset teaches
        // people to ignore the log. The paths are included because "which candidate should have existed" is the first
        // question when they are missing.
        HP_LOG_WARNING(kLogChannel, "{} — panels will draw text labels instead", message);
        return false;
    }

    m_SourcePath = chosen;
    // {} placeholders, not printf: HP_LOG_* formats with fmt, so "%s" passes through verbatim and the line that tells
    // someone where their icons came from says nothing.
    HP_LOG_INFO(kLogChannel, "icon atlas loaded from {} ({} icons)", m_SourcePath, m_Layout.GetIconCount());
    return true;
}

void EditorIconSet::Unload(UiRenderer& ui)
{
    if (m_TextureId != 0)
    {
        ui.UnregisterTexture(m_TextureId);
        m_TextureId = 0;
    }
    m_Texture = nullptr;
    m_SourcePath.clear();
}

}  // namespace Hollowpoint::Editor
