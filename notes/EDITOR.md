# Editor — what is built, and what is deliberately not

This file records editor gaps that are **decisions**, not oversights. AGENTS.md §1 requires exactly this: finish a step,
then state the boundary in `notes/` rather than leaving a placeholder that reads as finished work. Ten gaps are recorded below;
five code comments across `EditorApplication.h/.cpp` and `EditorPanels.cpp` point here by number (`see notes/EDITOR.md §3`), so
each is checkable against the section it names.

The shipped surface and its test coverage are in `docs/ARCHITECTURE.md`'s status table. Design reasoning — why the editor hosts
the runtime's loop, why ImGui has a custom backend, the vertex-offset bug that hid every panel — is Session 16 of
`notes/EXPERIMENTS.md`. This file is only about what an author cannot do yet.

---

## 1. Asset importing — the content browser lists, it does not import

`ContentBrowserModel::OpenEntry` opens `.hpscene` and `.lua`, and returns **false with a message naming the file** for
everything else (`no importer for "x.glb" yet`). A `.glb` therefore shows in the tree marked `unmanaged` and does nothing
on double-click except say so.

Why not accept glTF now: there is no asset database (planned row in `docs/ARCHITECTURE.md`'s status table), so an import
would have nowhere to record a GUID, no dependency graph to invalidate when a source file changes, and no cooked-output
format — all three are the open question "cook at import time vs cook-on-build" in that document's open decisions. Writing
a loader that silently re-reads source files on every load is the thing that later becomes impossible to remove.

**What it will take:** the GUID/path database first (planned edge under `Game` in ARCHITECTURE.md: "glTF via cgltf, texture
cook via stb, a GUID/path database"), then an import step that produces engine-owned vertex/index buffers so
`MeshRenderer::MeshPath` can name something other than the built-in shapes. The seam is already narrow: one function decides
"openable or not", and the content browser's Type column reports the answer rather than hiding it.

## 2. Play mode — edit mode only

`AreScriptsPaused()` returns `!PlayModeScripts`, and `PlayModeScripts` defaults to false. The Lua VM exists (so
`script.run` works on demand) but gameplay scripts do not tick while editing, which is the point: a world that moves under
the cursor is not an editor.

A working **Play** button needs more than flipping that flag — undo semantics for state a script changed, a way back to the
edited scene after scripts ran, and physics stepping that does not fight the inspector. Half-building it (a button that
resumes ticks with no restore path) is worse than no button, so it is not built. `--play` on the command line exists and is
tested as far as "scripts tick", which is honest about its scope.

## 3. Layout persistence — no ini file

`io.IniFilename = nullptr`. Panel arrangement comes from initial rectangles computed per frame (`PlaceInitially` in
`EditorPanels.cpp`, `ImGuiCond_FirstUseEver`) and an author's own arrangement lasts the session, then resets.

Why not write ImGui's ini now: it is one global file for every project, so two projects with different scenes fight over one
layout, and a stale ini referencing a panel that no longer exists is a support question with no good answer. Per-project
settings need a settings-file convention the engine does not have yet (no user-data path, no versioning, no atomic write).
When that exists, this becomes: point `IniFilename` at `<project>/.hpeditor/layout.ini`, and add the file to the project's
ignore list.

## 4. Profiling overlays — implot not integrated

`docs/ARCHITECTURE.md`'s decision 3 mentioned implot. Nothing in the editor graphs frame times, draw counts or memory yet;
the menu bar shows a frame counter and `editor.info` prints numbers as text.

Deliberately deferred rather than stubbed: an overlay needs a sampling source that is itself correct (a frame-time history
that includes present waits or it lies), and the engine's timing instrumentation is not finished. A pretty wrong graph is
the worst possible profiling tool.

## 5. Undo / redo — none

`CreateEntity`, `DestroySelected`, `entity.set` and `scene.save` are all immediate, with no history. The details panel writes
straight through `Reflection::WriteFieldValue` (deliberately: the same clamped path the loader uses, so an edit cannot change
on save/reload), which means there is no interception point where an undo record could be captured.

This is the highest-value missing editor feature and the one that constrains other designs: array-of-struct fields are shown
read-only in the inspector precisely because resizing them without undo is how an author loses work with no way back. An
undo stack wants either command objects (each mutation already has a name and arguments — the console registry is close) or
structural snapshots; the former fits this codebase, and `EditorConsole`'s records are nearly the right shape already.

## 6. Picking scale — linear scan, no BVH

`PickRay` transforms every triangle of every candidate per click. That is correct and fast enough to be free on a click for
scenes up to roughly tens of thousands of triangles, which is everything the engine renders today.

The broad phase exists (per-candidate AABB, `IntersectsRayBox`) but is per-candidate, not hierarchical. The comment in
`Picking.cpp` says why the bounds are recomputed rather than cached: a cache needs an invalidation rule (mesh changed?
transform changed?) whose bugs are worse than an O(vertices) loop on mouse-press. A BVH earns its keep when picking becomes
per-frame (marquee selection, hover highlighting on move) rather than per-click — that is the trigger to revisit, and
`PickCandidate`'s interface does not have to change when it happens.

## 7. Live layout reflow — viewport extent is estimated

The scene renders into a texture sized from the window minus side/bottom panel fractions, not from the actual dock-node
rectangle (`EnsureViewportTargets` call in `RenderScenePass`). Panels are drawn at that same computed size, so the image and
its widget cannot drift apart by an accumulating rounding term — but dragging a dock divider does not resize the scene until
the quantised estimate changes (16-pixel steps).

Why quantised at all: recreating two textures plus a pipeline per pixel of a drag is allocator churn and a stutter that reads
as a renderer bug. Reading the real dock-node rect is the fix; it needs the node's id queried after layout, which ImGui only
offers cleanly once the dockspace has settled (it is empty on frame 1, which is exactly when a size is needed).

## 8. Docked panels are not yet draggable into each other in every arrangement

Docking is enabled (`ImGuiConfigFlags_DockingEnable`) and a dockspace fills the window, so a panel title bar can be dragged
into nodes. Splitting an arbitrary node, tabbing two panels together and restoring that after a hide/show cycle are ImGui
behaviours that work but are untested here — the test suite asserts each panel draws its own contents in the default layout
(`TestEditorApplication.cpp`), not that a re-dock preserves it. Noted because "docking works" is a claim about interaction,
and interaction is what automated tests see least of.

## 9. Console parity — one command has no widget, several widgets have no arguments

Every panel action routes through `EditorConsole`, so the requirement in AGENTS.md ("every editor action will have a
scripting/console command equivalent") holds for what exists: `scene.load/save`, `entity.create/destroy/set/inspect`,
`content.list/open`, `viewport.camera`, `pick.at`, `screenshot`, `script.run`, `editor.select/clear/info/input/quit`,
`panels.show`.

Two honest asymmetries:
* The console takes a string where a widget would offer a constrained control (a checkbox, an enum combo). `entity.set`
  therefore tries bool words, then a number, then stores text — which is right for both `Visible false` and
  `roughness 0.2`, and fails at the reflection layer rather than storing zero when the kind does not match.
* `script.run` executes a Lua file; the sandboxed engine API in `engine/Scripting/.../EngineAPI.h` is the scripted path for
  scene work and is where richer agent operations belong. The editor console is a front end to engine code, not a second
  scripting host — it must not grow its own bindings that scripts cannot reach.

## 10. Multi-window / viewport-per-panel

One viewport panel, one camera (the tool's fly camera). Multiple viewports (4-quadrant UE-style, or a second window on
another display) each need their own HDR/depth pair, aspect and input routing; `EnsureViewportTargets` currently owns exactly
one set. Not architecturally blocked — the per-panel render path is already separate from the back buffer — but it is a
refactor of ownership (a list of viewports rather than four members) that should be done when there is a second one to serve,
not before.
