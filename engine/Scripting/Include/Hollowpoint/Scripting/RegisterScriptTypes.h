#pragma once

#include "Hollowpoint/Core/API.h"

// ---------------------------------------------------------------------------
// Registration of the scripting component with reflection and the scene factory.
//
// Why this lives in Scripting rather than in Game: `ScriptComponent` is declared here, and Game must not link Scripting
// — a build without Lua (HP_ENABLE_SCRIPTING=OFF) still builds Game, and a module that registered a type it cannot
// compile would make that configuration fail at configure time rather than being a supported option.
//
// Why registration is a function and not a static initialiser: the project's rule is that scene-loadable component
// types are registered explicitly by the host at startup (see Game::RegisterGameTypes), because a static initialiser
// registering into a singleton gives no control over ordering and no way to tell whether a type is available. SceneIO
// *skips* an unregistered component name silently — so without this call, `"Script": {"scriptName": "Spin"}` in a scene
// file produces an entity with no script, and the only clue is that nothing moves.
//
// The name in scene files is `Script`, matching how "Camera" and "Light" drop their C++ suffix; the field is
// `scriptName`.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Scripting
{

//! Registers `ScriptComponent` (scene-file name `Script`) with the reflection registry and the component factory.
//! Idempotent: safe to call from several hosts, and safe after a test or tool already registered it — re-registering
//! the same name is what the registry reports as an error, so this function checks first instead.
HP_SCRIPTING_API void RegisterScriptTypes();

//! True when `Script` is registered in both places a load needs (type descriptor *and* factory entry). A
//! half-registered type parses but instantiates nothing, which is the failure worth being able to ask about.
[[nodiscard]] HP_SCRIPTING_API bool AreScriptTypesRegistered();

}  // namespace Hollowpoint::Scripting
