# Editor UI/UX review — why it does not feel like Unreal Engine

Date: 2026-11-03. Method: read every editor source file, ran the real editor on this machine's GPU and analysed the
composed frame pixel by pixel (the engine's own `--screenshot` path), plus static checks for wiring. Every number below
comes from a command in this session, not from reading code and guessing.

Baseline captured with:

```
./build/debug/output/hollowpoint-editor --hidden --frames 8 --width 1600 --height 900 --screenshot /tmp/editor_now.png
```

Log line of interest: `viewport targets: 944x640`.

---

## 1. What is actually wrong, ranked by how much it hurts

### F1 — The viewport renders at a size its panel cannot show: the image is cropped and scrollable
`EditorApplication::RenderScenePass` sizes the HDR target from an arithmetic *estimate*
(`contentWidth - 660`, `contentHeight - 260`) while `DrawViewportPanel` draws the image at that same fixed pixel size.
Measured from the composed frame:

| | value |
|---|---|
| window content | 1600 x 900 |
| HDR/display target rendered | **944 x 640** (log) |
| region actually visible in the panel | **~810 x 584** |
| of the rendered image, visible | ~78% — **22% is clipped away** |
| scrollbars drawn over it | vertical + horizontal, ~19 px each |

The comment above that estimate claims "the Image widget is drawn at exactly the target size so the scene and its panel
cannot drift apart". That claim is false in both directions: the estimate is 134 px wider than the panel's content area,
so ImGui clips the image and adds scrollbars. It also means the scene is *projected* at aspect 944/640 = 1.475 while the
part you see is 810/584 = 1.39 — so the perspective you author against is not the perspective you see.

This is the single most visible "this is a toy editor" artefact, and it is exactly what `notes/EDITOR.md` §7 describes
as an accepted 16-pixel quantisation error. It is not a quantisation error; it is a constant ~130 px overshoot.

### F2 — Default ImGui look: 13 px un-antialiased bitmap font, stock palette, no density settings
Nothing in the editor calls `StyleColors*`, `AddFont*` or any `StyleVar`. Consequences measured in the capture:

* The main menu bar strip (1600 x 18) contains **exactly 3 distinct colours** — text is drawn with no anti-aliasing.
  ImGui 1.93 picks `AddFontDefaultBitmap` (ProggyClean) when `FontSizeBase * FontScaleMain * FontScaleDpi *
  io.DisplayFramebufferScale < 15`; at base 13 and framebuffer scale 1 that is the crunchy pixel font, always.
* Title bars are ImGui's stock saturated blue `(0.16, 0.29, 0.48)` — the single most recognisable "this is a raw ImGui
  demo window" signal there is. UE uses flat near-black chrome (`#2F2F2F`-family) with a thin accent on the active item.
* Panel geometry is default: `WindowPadding` 8, `FramePadding` (4,3), `ItemSpacing` (8,4), `WindowRounding` 0,
  tab/separator/hit-spacing at defaults. UE's docked chrome is denser in places and roomier in others; the default
  combination reads as a debug menu, not an editor shell.
* There is no DPI/user scale story at all: at framebuffer scale 2 everything stays at 13 px virtual unless someone
  happens to have a HiDPI display, where it silently changes.

### F3 — Panels are four free-floating windows *over* a dockspace, not docked in it
`DrawEditorPanels` calls `DockSpaceOverViewport` and then five plain `ImGui::Begin` windows positioned with
`ImGuiCond_FirstUseEver`. Nothing ever docks them into the tree (`grep -rn DockBuilder engine/` → no hits), so:

* each panel carries a 19 px title bar (5 panels = 95 px of chrome stolen from the work area, ~10% of a 900 px window);
* dragging one panel's divider does not resize its neighbours — they are independent windows;
* the whole arrangement can be scattered into an unrecoverable mess with **no way to get it back** (no reset, no ini);
* "Reset Layout" — the menu item every UE user reaches for first — does not exist.

### F4 — The viewport is a picture, not a viewport: no camera, no click-to-select, no gizmos
`grep` over the whole editor for mouse handling in panels returns two hits, both in the content browser (a tooltip and
a double-click). Specifically:

* `EditorApplication::PickAtViewportPixel` — the ray picker, fully implemented and tested — has **exactly one caller:
  the `pick.at` console command**. Clicking an object in the viewport cannot select it.
* No key or mouse binding moves the view camera. `SetViewportCamera` is likewise only reachable from
  `viewport.camera <x y z>`. The editor opens at a fixed three-quarter view and stays there forever: you cannot walk
  around the scene. This alone makes the tool unusable for placement, which is an editor's primary job.
* No RMB-fly / MMB-orbit / wheel-dolly (UE defaults), no `F` = frame selection, no transform gizmo, no context menu
  anywhere in the viewport, no mode/limits toolbar, no reference grid overlay in view.

### F5 — The inspector fights you where it should help
* Every field is a bare `DragFloat`/`InputInt` with default speed and no range: `Position.x` and `Roughness` behave
  identically (0.01/frame, unbounded), which is wrong for both.
* Enum fields (`MeshCullMode`, `LightType`, `ProjectionType`) render as **integer spinners**. The reflection registry
  carries the entry names — `EnumDescriptor::FindNameByValue` exists and is used by the serialiser — so showing `0`
  instead of `Back`/`Front`/`None` is a lost call, not a missing capability.
* Labels sit above controls (ImGui default) rather than UE's label-left/value-right rows, so a component reads as a
  long column instead of a table and wastes vertical space.
* Array-of-struct fields show `Name (N entries)` with no expansion — defensible without undo, but it is still a gap.

### F6 — No persistence, no undo, no status bar, no toolbar
`io.IniFilename = nullptr` with no reset command means every session starts from the hard-coded default and an author's
arrangement cannot even be recovered within the session. There is no toolbar (Save/Play/Snap), no status bar (the frame
counter is squeezed into the menu bar, and `PanelContext::SceneDrawCalls` is computed every frame and **never drawn by
anyone**), and no undo of any kind (`notes/EDITOR.md` §5 — agreed highest-value gap).

### F7 — Small things that add up
* The console's Tab-completion reads `state.ConsoleInput`, which is only written when the text box *commits*, so Tab
  completes the previous line — the feature cannot work as written.
* The outliner's rows are a flat list of `Name | Slot`; UE shows a tree with icons and hides slot numbers, which no
  author reads.
* Content browser prints its root path in raw text and marks everything it cannot import `unmanaged`.

---

## 2. What the test suite did and did not catch

`tests/Editor/TestEditorApplication.cpp` asserts each panel region contains structure (distinct-colour counts) and that
adding an entity changes >0.05% of pixels. Both are good checks and both passed on a frame whose viewport was cropped by
22% with two scrollbars over it, because "the region contains structure" is satisfied by any non-flat rectangle —
including a scrollbar plus clipped image. **The suite verifies that panels draw; nothing verifies that they fit.**

---

## 3. Fixes applied in this session (see git log; before/after numbers at the bottom)

1. Viewport sized from its panel's real content area, drawn to fill it (`GetContentRegionAvail`), never clipped, no
   scrollbars; scene projection now uses the aspect you actually see.
2. `EditorStyle`: UE-ish palette + metrics + a real TTF font with an explicit UI scale, applied before the first frame
   and rebuilt when the display scale changes.
3. Real docked layout built with `DockBuilder` (no per-panel title bars), plus `Window > Reset Layout` and
   `layout.reset` / `layout.report`.
4. Viewport interaction: RMB fly (WASD/QE, Shift sprint), MMB orbit/pan, wheel dolly, left-click pick, `F` frame
   selection — all gated on pointer-over-viewport so widgets keep their input, and all reachable from the console.
5. Inspector: label-left rows, enum combos fed from reflection, per-kind drag speeds/formats.
6. Console Tab-completion fixed; status bar with draw calls, entity/selection counts, camera position.

## 4. Still open after this session (deliberate)

Undo/redo (blocks gizmo drag semantics and array editing); transform gizmos drawn in-scene (needs a line/point
pipeline — the viewport has no overlay pass yet); Play mode with restore; asset import + content tree; per-project
layout persistence to disk (needs a settings-file convention, `notes/EDITOR.md` §3); implot overlays.

---

## 5. Before / after (measured, same capture path: `--hidden --frames N --width 1600 --height 900`)

| metric | before | after | how measured |
|---|---|---|---|
| viewport target vs visible region | 944x640 rendered into ~809x585 → **22% cropped**, 2 scrollbars | **896x560 target / 900x565 displayed** — within one 16px quantisation step, no crop | `viewport targets:` log line vs. anchored flood of the composed PNG |
| scene image size | 809 x 585 (32.9% of window) | 900 x 565 (35.3%), and it now *tracks the dock node* when a splitter is dragged | same |
| projection aspect vs displayed aspect | 1.475 projected vs 1.39 seen (**6% mismatch**) | equal by construction (target extent == displayed extent ± quantum) | §F1 arithmetic on measured extents |
| UI font | ImGui embedded **bitmap** ProggyClean, 13px | **Cantarell-Regular.otf at 15px**, vector, anti-aliased | log line `ui font … at 15px`; menu-bar strip colours 3 → 8 (anti-aliasing produces intermediate values a bitmap font cannot) |
| panel chrome | 5 floating windows × 19px title bars ≈ **95px** stolen; dragging one edge moved nothing | real dock tree, no per-panel title bars; splitters resize neighbours | `layout.report` rectangle dump |
| layout recoverable after scattering | **no** — no reset existed | `Window > Reset Layout`, `layout.reset` | new test `layout.reset rebuilds the docked tree…` |
| camera navigation | none (console only) | RMB fly WASD/QE + Shift sprint, MMB orbit, Shift+MMB pan, wheel dolly, F frame | 13 unit tests in `EditorViewCamera`, mutation-verified |
| click-to-select in viewport | **never** (`PickAtViewportPixel` had one caller: a console command) | left-click picks via the same entry point | code path + `pick.at` unchanged |
| enum properties | integer spinner showing `0` | combo showing `Back`/`Front`/`None`, from reflection | DrawDetailsPanel enum branch |
| inspector layout | label above, one drag speed for everything | UE label-left/value-right rows, per-field speeds | BeginFieldRow / DragSpeedFor |
| status bar | none; `SceneDrawCalls` computed every frame and drawn by nobody | bottom strip: draw calls, entity/selection counts, camera xyz, fps | DrawStatusBar |
| console Tab-completion | completed the **previous** line (read a buffer only written on commit) | completes the live buffer via ImGui's completion callback | ConsoleCompletionCallback |
| `--command` output | invisible (transcript lived only in the panel) — every automated check had to be inferred from pixels | echoed to stdout with reply/error | Main.cpp; see §6 |

### Test-side changes were necessary, not optional
The pre-existing suite asserted "panel region contains ≥3 distinct colours". Mutation testing showed **hiding the
outliner entirely left every one of those assertions green** — fractional pixel regions cannot attribute content to a
dock panel. Those checks are replaced by geometry assertions on `CollectPanelRectangles()` (a hidden or unsized panel
reports 0x0), plus structure counted *inside* that rectangle. Every new assertion was mutation-checked:

| injected defect | caught? |
|---|---|
| outliner never draws | yes (2 failures) |
| viewport sized by the old arithmetic estimate (the crop regression) | yes |
| `layout.reset` does not rebuild the tree | yes |
| pan moves along the view axis instead of within it | yes |
| dolly linear instead of multiplicative | yes |
| fly without diagonal normalisation | yes |
| view basis right-axis handedness flipped | yes |
| FrameBounds frames from the wrong side of the ray | yes (after adding an assertion; it initially survived) |
| FrameBounds validation removed entirely / overflow clause dropped | yes (15 / 6 failures) |

Two tests written during this session were **vacuous and got rewritten**, which is the honest record: a "pitch stays
clamped" test could not fail because `atan2` bounds the angle regardless, and a "pan never accumulates roll" test
re-asserted `GetViewBasis`'s own definition. Both replaced with properties that can actually fail; the reasoning is in
comments at each site so the next reader does not re-add the tautology.

## 6. Tooling gaps that made this harder than it needed to be

* **`--command` output was unreadable from outside the editor.** `Main.cpp` discarded `RunStartupCommand`'s result and
  the console transcript lived only in the panel, so `--command layout.report` produced nothing anyone could read — which
  is why UI correctness had been inferred from screenshot pixels. Commands now echo their transcript lines to stdout (an
  earlier version of that fix double-printed every command that returns no reply, e.g. `editor.quit`).
* **The build-target trap** (cost hours): the editor executable target is `HollowpointEditorApp`; building
  `--target HollowpointEditor` updates only the static library and leaves `output/hollowpoint-editor` stale, so a fix can
  appear to do nothing across many iterations while the logs still look current. Recorded in the environment profile.
* **Measurement traps hit and corrected mid-session.** Reading glyph coverage from the font atlas's *red* channel (ImGui
  stores coverage in alpha) reported "the atlas is binary, so the font is not anti-aliased"; a hardcoded menu-bar row
  range measured a different UI element on a differently-sized window and reported three colours on a frame whose text was
  demonstrably smooth. Both conclusions were wrong, and both were re-derived from data before acting on them — recorded
  because the plausible-sounding wrong answer came from the tooling, not from the editor, and it nearly produced a fix to
  a bug that did not exist.

## 7. Defects found *while verifying the fix* (each caught by mutation-testing my own work)

These were not visible in the code and are worth recording because each is a class of mistake, not a one-off typo.

1. **The existing panel tests were vacuous.** "Count distinct colours inside a fractional region" passed with
   `DrawOutlinerPanel` returning immediately — docked panels tile the window, so a region can be satisfied by a
   neighbour's fill or the dock background. Replaced with assertions on docked-window geometry (`CollectPanelRectangles`).
2. **A test I wrote for this fix was itself vacuous.** `layout.reset`'s assertion checked the panel's rectangle after
   reset, but ImGui keeps a hidden window's *last* rectangle — so it passed whether or not the panel had been restored.
   Fixed by asserting the visibility flag and by making geometry report 0x0 for windows that were not submitted this
   frame (`Hidden || !WasActive`), then mutation-checking both directions (reset-without-restore → 3 failures; stale
   rectangles → 1 failure).
3. **Two more of my own tests could not fail** and were rewritten: "pitch stays clamped" (bounded by `atan2` regardless
   of the clamp) and "pan never accumulates roll" (re-asserted `GetViewBasis`'s own definition — a tautology). Both now
   assert saturation / displacement-direction properties that do fail when broken; the reasoning is inline so nobody
   re-adds the tautology.
4. **A comment argued for metrics it did not implement.** `ComputeEditorStyle` claimed its padding was "slightly tighter
   than ImGui"; ImGui's defaults are FramePadding (4,3), ItemSpacing (8,4), CellPadding (4,2) and mine are *larger*
   (7,4 / 8,5 / 5,3). Verified by reading `ImGuiStyle`'s constructor rather than from memory, and the comment now states
   which metrics go each way and why.
5. **`layout.reset` did not restore hidden panels.** It rebuilt the dock tree while leaving anything toggled off through
   the Window menu still invisible — a "reset layout" that still hides half the UI. Reset now restores visibility too.
6. **`--command` output was unreadable from outside the editor** (see §6), which is *why* so much of this had to be
   inferred from pixels; fixed, and the transcript is now the single printed source (an earlier version double-printed
   every command with no reply).

## 8. Reproducing the measurements

```sh
cmake --build build/debug -j8                      # NOT --target HollowpointEditor: that is the library only
./build/debug/output/hollowpoint-editor --hidden --frames 20 --width 1600 --height 900 \
    --screenshot /tmp/final.png --no-validation
python3 scripts/analyze_editor_frame.py /tmp/final.png
ctest --test-dir build/debug -R EditorTests --output-on-failure
python3 scripts/check_doc_counts.py                # repo gate: documented test counts vs sources
```
