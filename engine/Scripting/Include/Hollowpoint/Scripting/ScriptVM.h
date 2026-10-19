#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

struct lua_State;

namespace Hollowpoint::Scripting
{

// ---------------------------------------------------------------------------
// ScriptVM — a single Lua state with the engine's safety policy applied.
//
// Why a VM object rather than a global: tools (asset builders, tests) need several isolated states, and a test suite
// that shares one VM leaks every script it loads into the next test, making failures depend on test order. One VM per
// runtime context (game, editor preview, tool) is also what lets a misbehaving script be discarded wholesale.
//
// Safety policy, all of it load-bearing:
//
//   * **Every entry point is protected.** A Lua runtime error must never propagate into C++ as an exception that skips
//     engine invariants (or worse, terminate the game). Errors are caught, reported with a traceback, and the VM stays
//     usable — a bug in one gameplay script costs that frame's behaviour, not the session. This was verified against
//     the vendored Lua/sol2 before this class was written: an erroring handler called repeatedly leaves the VM
//     functional for everything else.
//   * **A memory budget.** Scripts can allocate without bound (a runaway loop filling a table); a custom allocator caps
//     the heap so a bug becomes a Lua error rather than the OS killing the process. Verified: a script allocating
//     megabytes is contained at the configured limit and the VM remains usable afterwards.
//   * **No dangerous libraries.** `os` and `io` are not opened, and neither is `load`/`dofile`/`require` from the base
//     library set beyond what the sandbox needs. Gameplay scripts are content, and content that can shell out to the OS
//     or overwrite files is a bug with no upside; engine APIs are added deliberately instead.
//   * **No C++ types exposed as userdata.** Entities are passed as opaque integer handles (slot + generation packed),
//     and components are reached through functions, not by handing out references. Exposing live engine objects as
//     usertypes ties their lifetime to Lua's garbage collector: an entity destroyed by the world leaves a script-side
//     reference pointing at recycled memory, and every method call becomes a use-after-free that only the GC's timing
//     decides. Handles make "is this still alive?" a single check the engine controls.
// ---------------------------------------------------------------------------

class ScriptVM
{
public:
    //! Byte budget for the VM's heap. Exceeding it raises a Lua error inside the running script rather than letting
    //! the process grow unbounded. Sized generously: enough for real gameplay data, small enough that an accidental
    //! infinite allocation is caught in a playtest instead of on a player's machine.
    static constexpr std::size_t DefaultMemoryLimitBytes = 64u * 1024u * 1024u;

    ScriptVM();
    ~ScriptVM();

    ScriptVM(const ScriptVM&) = delete;
    ScriptVM& operator=(const ScriptVM&) = delete;
    ScriptVM(ScriptVM&&) noexcept;
    ScriptVM& operator=(ScriptVM&&) noexcept;

    //! True when the Lua state was created and the engine API is registered.
    [[nodiscard]] bool IsInitialized() const noexcept;

    //! Loads and runs a chunk (a file's contents). Returns false and fills `outError` with a positioned message on a
    //! compile or runtime error; the VM remains usable either way.
    bool Run(std::string_view code, std::string& outError, std::string_view chunkName = "chunk");

    //! Looks up a global function by name. Returns nullptr when absent or not callable — scripts are optional by
    //! design (an entity with no script attached is normal), so "no such function" is not an error condition.
    [[nodiscard]] bool HasFunction(std::string_view name) const;

    //! Calls a global function with (entityHandle, deltaTimeSeconds). Returns false if the call raised an error, which
    //! has already been reported through the log so callers need not treat it as fatal.
    bool CallUpdate(std::string_view functionName, std::int64_t entityHandle, float deltaTimeSeconds);

    //! Total bytes currently allocated by the VM's allocator. Exposed for the memory HUD and for tests that assert a
    //! budget is enforced; also the only way to notice a script leaking before it reaches the cap.
    [[nodiscard]] std::size_t GetMemoryUsageBytes() const noexcept;

    [[nodiscard]] std::size_t GetMemoryLimitBytes() const noexcept;

    //! Number of errors caught so far. Surfaced so tools and tests can assert "this ran clean" without scraping logs,
    //! and so a debug overlay can show that scripts are erroring every frame rather than failing silently.
    [[nodiscard]] std::uint64_t GetErrorCount() const noexcept;

    //! The raw Lua state, for the API layer and tests that need the C API (sol2 exposes no allocator hook, so the VM
    //! is created with `lua_newstate` and viewed through sol). Never null once IsInitialized() is true.
    [[nodiscard]] lua_State* GetLuaState() noexcept;

    [[nodiscard]] const lua_State* GetLuaState() const noexcept;

    //! Reports an error through the engine log with the VM's accumulated context, and increments the error counter.
    //! Central so every failure path reports identically rather than each call site inventing a format.
    void ReportError(std::string_view message);

    // -----------------------------------------------------------------------
    // Engine-owned state attached to this VM.
    //
    // The API layer allocates per-VM context (the world pointer, prefab resolver, transform cache) and every registered
    // function references it. That context must be destroyed by **us**, not by Lua: storing it inside a capturing sol2
    // closure makes it a GC finalizer's responsibility, so it can run during lua_close — after the World it points at
    // may already be gone — and in an order no caller controls. Registering the destruction here makes teardown
    // deterministic and ordered before lua_close.
    //
    // Type-erased with an explicit deleter so this class does not depend on what the API layer allocates; replacing a
    // context destroys the previous one immediately, which is what re-registration needs (the old `hp` table is
    // replaced wholesale, so its closures can never be called again).
    // -----------------------------------------------------------------------

    void SetEngineAPIContext(void* context, void (*destroy)(void*) noexcept) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;

    void* m_EngineAPIContext = nullptr;
    void (*m_DestroyEngineAPIContext)(void*) noexcept = nullptr;
};

}  // namespace Hollowpoint::Scripting
