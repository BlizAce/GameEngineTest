#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Editor/EditorIcons.h"

// ---------------------------------------------------------------------------
// Icon atlas tests — CPU-only, but they read the *shipped asset*.
//
// The reason this suite exists at all: the atlas PNG, its manifest and the C++ name table are three artefacts produced
// by one generator (scripts/make_editor_icons.py). Any of them can go stale on its own — someone edits the PNG by hand,
// or regenerates it and forgets to commit the .inl — and every such mismatch is silent at runtime: buttons keep drawing,
// just the wrong picture. These cases make each drift a test failure instead.
//
// They also pin the two geometry facts everything else rests on: cell arithmetic (UVs derived from index) and image
// orientation (top-down rows). An inverted atlas does not crash; it renders every glyph upside down, which at 16 px
// looks like bad iconography rather than a bug.
// ---------------------------------------------------------------------------

namespace
{

using namespace Hollowpoint;
using Editor::DecodedIconAtlas;
using Editor::IconAtlasLayout;
using Editor::IconUvRect;

//! Repo root, found by walking up from the working directory until assets/editor/icons exists.
/**
 * The test binary runs from wherever ctest is invoked (build/debug here, repo root there), so a hard-coded relative path
 * would make this suite pass or skip depending on the caller — the worst property a drift guard can have. Walking up is
 * deterministic and skips honestly when the asset genuinely is not present (an installed test-only package).
 */
[[nodiscard]] std::filesystem::path LocateRepoRoot()
{
    std::filesystem::path directory = std::filesystem::current_path();
    for (int depth = 0; depth < 8 && !directory.empty(); ++depth)
    {
        if (std::filesystem::exists(directory / "assets/editor/icons/hp_editor_icons.png"))
        {
            return directory;
        }
        directory = directory.parent_path();
    }
    return {};
}

[[nodiscard]] std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

//! Names and cell coordinates in icon_manifest.json, parsed by hand.
struct ManifestEntry
{
    std::string Name;
    int X = 0;
    int Y = 0;
};

[[nodiscard]] std::vector<ManifestEntry> ParseManifest(const std::string& json)
{
    // Deliberately string-scanning rather than pulling in a JSON dependency for one test: the manifest is written by our
    // own generator with a fixed shape ("name": "x", ... "x": 32, "y": 64), and an object-per-icon block boundary is
    // enough to keep name/x/y associated. A parser would be more general and less honest about how tightly this test is
    // coupled to the generator's output format — which is exactly the coupling we want it to catch breaking.
    std::vector<ManifestEntry> entries;
    std::size_t searchFrom = 0;
    while (true)
    {
        const std::size_t nameKey = json.find("\"name\"", searchFrom);
        if (nameKey == std::string::npos)
        {
            break;
        }
        const std::size_t openQuote = json.find('"', json.find(':', nameKey));
        const std::size_t closeQuote = openQuote == std::string::npos ? std::string::npos : json.find('"', openQuote + 1);
        if (closeQuote == std::string::npos)
        {
            break;
        }

        ManifestEntry entry;
        entry.Name = json.substr(openQuote + 1, closeQuote - openQuote - 1);

        const std::size_t blockEnd = json.find("\"name\"", closeQuote);
        const std::string block = json.substr(closeQuote, blockEnd == std::string::npos ? std::string::npos
                                                                                        : blockEnd - closeQuote);
        for (const char* key : {"\"x\"", "\"y\""})
        {
            const std::size_t at = block.find(key);
            if (at == std::string::npos)
            {
                continue;
            }
            int value = 0;
            std::sscanf(block.c_str() + at + std::string_view(key).size(), " : %d", &value);
            if (key[1] == 'x')
            {
                entry.X = value;
            }
            else
            {
                entry.Y = value;
            }
        }

        entries.push_back(entry);
        searchFrom = closeQuote + 1;
    }
    return entries;
}

[[nodiscard]] int OpaquePixelsInCell(const DecodedIconAtlas& atlas, int x, int y, int cell)
{
    int count = 0;
    for (int row = 0; row < cell; ++row)
    {
        for (int column = 0; column < cell; ++column)
        {
            const std::size_t pixel = (static_cast<std::size_t>(y + row) * atlas.Width + (x + column)) * 4u;
            if (atlas.Rgba[pixel + 3] > 16)
            {
                ++count;
            }
        }
    }
    return count;
}

//! A tiny RGBA image built from an explicit alpha mask, for the bleed tests.
[[nodiscard]] DecodedIconAtlas MakeTestAtlas(int width, const std::vector<std::string>& asciiAlpha,
                                             std::uint8_t red = 200)
{
    DecodedIconAtlas atlas;
    atlas.Width = static_cast<Core::UInt32>(width);
    atlas.Height = static_cast<Core::UInt32>(asciiAlpha.size());
    atlas.Rgba.assign(static_cast<std::size_t>(width) * asciiAlpha.size() * 4u, 0);
    for (std::size_t y = 0; y < asciiAlpha.size(); ++y)
    {
        for (std::size_t x = 0; x < asciiAlpha[y].size() && x < static_cast<std::size_t>(width); ++x)
        {
            const std::size_t pixel = (y * static_cast<std::size_t>(width) + x) * 4u;
            if (asciiAlpha[y][x] == '#')
            {
                atlas.Rgba[pixel + 0] = red;
                atlas.Rgba[pixel + 1] = 40;
                atlas.Rgba[pixel + 2] = 30;
                atlas.Rgba[pixel + 3] = 255;
            }
        }
    }
    return atlas;
}

}  // namespace

TEST_SUITE("EditorIcons")
{
    TEST_CASE("the layout table is well-formed and its UVs follow cell arithmetic")
    {
        const IconAtlasLayout& layout = IconAtlasLayout::Default();

        CHECK(layout.GetAtlasWidth() == 512u);
        CHECK(layout.GetAtlasHeight() == 512u);
        CHECK(layout.GetCellSize() == 32u);
        CHECK(layout.GetIconCount() > 40u);  // a real icon set, not a stub

        std::set<std::string_view> names;
        for (Core::Size index = 0; index < layout.GetIconCount(); ++index)
        {
            const std::string_view name = layout.GetName(index);
            INFO("index " << index);
            CHECK_FALSE(name.empty());
            CHECK(names.insert(name).second);  // duplicates would silently alias two buttons to one glyph

            IconUvRect rect;
            REQUIRE(layout.Find(name, rect));
            CHECK(rect.U1 > rect.U0);
            CHECK(rect.V1 > rect.V0);
            CHECK(rect.U0 >= 0.0f);
            CHECK(rect.V1 <= 1.0f + 1e-6f);

            // The UV box must be exactly one cell wide/tall — a stretched cell draws part of its neighbour.
            const float cellU = 32.0f / static_cast<float>(layout.GetAtlasWidth());
            const float cellV = 32.0f / static_cast<float>(layout.GetAtlasHeight());
            const float widthError = std::abs(rect.Width() - cellU);
            const float heightError = std::abs(rect.Height() - cellV);
            CHECK(widthError <= 1e-5f);
            CHECK(heightError <= 1e-5f);

            // And it must sit on a cell boundary: fractional origins sample across two glyphs.
            const float boundary = std::fmod(rect.U0 * static_cast<float>(layout.GetAtlasWidth()), 32.0f);
            CHECK(boundary <= 1e-3f);
        }
    }

    TEST_CASE("every icon the UI asks for by name exists in the table")
    {
        // Names are string literals scattered through EditorPanels.cpp; a typo there is invisible until someone notices a
        // button with no picture. Listing them here keeps one authoritative inventory of what the chrome depends on.
        static const char* const kUsedByChrome[] = {
            "actor_cube",   "close_x",     "save",      "play",         "pause",         "stop",           "view_bookmark",
            "settings",     "search",      "panel_outliner", "panel_details", "panel_content", "panel_output_log",
            "actor_empty",  "actor_light", "actor_camera", "actor_script", "eye_open",      "eye_closed",
            "asset_scene",  "asset_script", "asset_mesh", "asset_texture", "asset_generic",
        };

        const IconAtlasLayout& layout = IconAtlasLayout::Default();
        for (const char* name : kUsedByChrome)
        {
            INFO(name);
            IconUvRect rect;
            CHECK(layout.Find(name, rect));
        }
    }

    TEST_CASE("the C++ table matches the shipped manifest exactly")
    {
        const std::filesystem::path root = LocateRepoRoot();
        if (root.empty())
        {
            WARN("assets/editor/icons not found from the working directory — skipping drift check");
            return;
        }

        const std::string manifestPath = (root / "assets/editor/icons/icon_manifest.json").string();
        const std::vector<ManifestEntry> manifest = ParseManifest(ReadFile(manifestPath));
        const IconAtlasLayout& layout = IconAtlasLayout::Default();
        REQUIRE_FALSE(manifest.empty());
        REQUIRE(static_cast<Core::Size>(manifest.size()) == layout.GetIconCount());

        // Order and coordinates, not just membership: the atlas' cells are positional, so a reordering that keeps the same
        // set of names still mislabels every glyph.
        for (std::size_t index = 0; index < manifest.size(); ++index)
        {
            INFO("manifest entry " << index << " '" << manifest[index].Name << "'");
            CHECK(layout.GetName(index) == manifest[index].Name);

            Core::UInt32 x = 0;
            Core::UInt32 y = 0;
            REQUIRE(layout.FindCell(manifest[index].Name, x, y));
            CHECK(static_cast<int>(x) == manifest[index].X);
            CHECK(static_cast<int>(y) == manifest[index].Y);
        }
    }

    TEST_CASE("the shipped atlas decodes top-down with ink in every named cell")
    {
        const std::filesystem::path root = LocateRepoRoot();
        if (root.empty())
        {
            WARN("asset not found — skipping");
            return;
        }

        DecodedIconAtlas atlas;
        std::string error;
        REQUIRE(Editor::DecodeIconAtlasPng((root / "assets/editor/icons/hp_editor_icons.png").string(), atlas, &error));
        INFO(error);
        CHECK(atlas.Width == 512u);
        CHECK(atlas.Height == 512u);

        const IconAtlasLayout& layout = IconAtlasLayout::Default();
        for (Core::Size index = 0; index < layout.GetIconCount(); ++index)
        {
            Core::UInt32 x = 0;
            Core::UInt32 y = 0;
            REQUIRE(layout.FindCell(layout.GetName(index), x, y));
            const int ink = OpaquePixelsInCell(atlas, static_cast<int>(x), static_cast<int>(y), 32);

            // Lower bound: a cell with almost no ink is empty or misaligned. Upper bound: a fully-opaque cell means the
            // atlas lost its transparency — which is exactly what happened once (an empty GIMP selection makes
            // edit_fill paint the whole layer), and it looks like "icons have boxes behind them" rather than a bug.
            INFO("icon '" << layout.GetName(index) << "' at " << x << "," << y << " ink=" << ink);
            CHECK(ink > 30);
            CHECK(ink < 900);
        }

        // Orientation, pinned with an asymmetric glyph: undo's arc sits in the cell's upper half by construction, so if
        // rows came back bottom-up the ink would be concentrated at the bottom instead.
        Core::UInt32 x = 0;
        Core::UInt32 y = 0;
        REQUIRE(layout.FindCell("undo", x, y));
        int topHalf = 0;
        int bottomHalf = 0;
        for (int row = 0; row < 32; ++row)
        {
            for (int column = 0; column < 32; ++column)
            {
                const std::size_t pixel = (static_cast<std::size_t>(y + row) * atlas.Width + (x + column)) * 4u;
                if (atlas.Rgba[pixel + 3] > 16)
                {
                    (row < 16 ? topHalf : bottomHalf) += 1;
                }
            }
        }
        CHECK(topHalf > bottomHalf);
    }

    TEST_CASE("alpha bleeding pushes colour into transparent texels without touching alpha")
    {
        // A red square with a one-texel transparent moat, then deeper transparency. The moat must take the red; the
        // far texels must still be black-ish at pass 1 (they are two away) and reach it by pass 2.
        DecodedIconAtlas atlas = MakeTestAtlas(7, {".......", ".###...", ".....#.", ".......", ".......", ".......", "......."});
        const std::size_t moat = (0 * 7 + 1) * 4u;   // texel left of the square's top-left corner
        CHECK(atlas.Rgba[moat + 3] == 0);
        CHECK(atlas.Rgba[moat + 0] == 0);

        Editor::BleedAtlasAlpha(atlas);

        CHECK(atlas.Rgba[moat + 0] == 200);  // colour bled in
        CHECK(atlas.Rgba[moat + 1] == 40);
        CHECK(atlas.Rgba[moat + 3] == 0);    // alpha untouched — otherwise every cell becomes a solid rectangle

        // An opaque texel is never modified.
        const std::size_t solid = (1 * 7 + 2) * 4u;
        CHECK(atlas.Rgba[solid + 0] == 200);
        CHECK(atlas.Rgba[solid + 3] == 255);
    }

    TEST_CASE("bleeding reaches up to the pass cap and no further")
    {
        // One opaque texel in a 24x24 field. Dilation spreads one Chebyshev ring per pass, so everything within 8
        // texels takes the colour and anything beyond stays black — the algorithm's honest limit, pinned here so a
        // future change to the cap is a deliberate edit rather than a silent behaviour shift.
        std::vector<std::string> field(24, std::string(24, '.'));
        field[12][12] = '#';
        DecodedIconAtlas atlas = MakeTestAtlas(24, field);
        Editor::BleedAtlasAlpha(atlas);

        const auto pixelAt = [&atlas](int x, int y) { return (static_cast<std::size_t>(y) * 24 + x) * 4u; };

        // Distance 1 and distance 8: both reached.
        CHECK(atlas.Rgba[pixelAt(13, 12) + 0] == 200);
        CHECK(atlas.Rgba[pixelAt(20, 12) + 0] == 200);
        CHECK(atlas.Rgba[pixelAt(20, 12) + 3] == 0);  // colour only; alpha stays transparent

        // Distance 12 (the corner): out of reach, so it must still be black.
        CHECK(atlas.Rgba[pixelAt(0, 0) + 0] == 0);
    }

    TEST_CASE("a degenerate atlas is rejected rather than uploaded")
    {
        DecodedIconAtlas bad;
        bad.Width = 4;
        bad.Height = 4;
        bad.Rgba.assign(4u * 4u * 3u, 0);  // short by one row
        CHECK_FALSE(bad.IsValid());

        // Bleeding a broken atlas must not read out of bounds.
        Editor::BleedAtlasAlpha(bad);

        DecodedIconAtlas empty;
        Editor::BleedAtlasAlpha(empty);
        CHECK_FALSE(empty.IsValid());
    }

    TEST_CASE("decoding a missing or non-image file fails with a reason")
    {
        DecodedIconAtlas atlas;
        std::string error;
        CHECK_FALSE(Editor::DecodeIconAtlasPng("/nonexistent/hp_editor_icons.png", atlas, &error));
        CHECK_FALSE(error.empty());
        CHECK_FALSE(atlas.IsValid());

        // A file that exists but is not a PNG: the same path must fail cleanly rather than produce a zero-size texture.
        const std::filesystem::path junk = std::filesystem::temp_directory_path() / "hp_not_a_png.png";
        {
            std::ofstream out(junk, std::ios::binary);
            out << "this is definitely not a PNG";
        }
        CHECK_FALSE(Editor::DecodeIconAtlasPng(junk.string(), atlas, &error));
        CHECK_FALSE(error.empty());
        std::filesystem::remove(junk);
    }

    TEST_CASE("icon size tracks UI scale and never collapses below one pixel")
    {
        CHECK(Editor::IconSizeForScale(1.0f) == doctest::Approx(16.0f));
        CHECK(Editor::IconSizeForScale(2.0f) == doctest::Approx(32.0f));
        CHECK(Editor::IconSizeForScale(0.5f) == doctest::Approx(8.0f));
        // Absurd and hostile scales clamp rather than returning 0 (an invisible icon) or a negative size (a flipped draw).
        CHECK(Editor::IconSizeForScale(0.0f) >= 16.0f);
        CHECK(Editor::IconSizeForScale(-5.0f) >= 16.0f);
        CHECK(Editor::IconSizeForScale(0.5f, 24.0f) == doctest::Approx(24.0f));
    }
}
