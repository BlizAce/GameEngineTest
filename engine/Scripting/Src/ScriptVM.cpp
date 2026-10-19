#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include <sol/sol.hpp>

#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Scripting/ScriptVM.h"

// Lua's headers are C. Including them inside extern "C" keeps this translation unit honest regardless of whether the
// vendored headers already carry the guard, and it is what lets us call lua_newstate with our own allocator.
#include "Hollowpoint/Scripting/LuaInclude.h"

namespace Hollowpoint::Scripting
{

namespace
{

//! Bookkeeping for the VM's allocator. One per VM, owned by ScriptVM::Impl and passed as the allocator's userdata.
struct AllocatorState
{
    std::size_t LiveBytes = 0;
    std::size_t LimitBytes = ScriptVM::DefaultMemoryLimitBytes;

    //! Peak usage, kept so a memory HUD can report high-water rather than only the current value (a leak's peak is what
    //! a developer needs to see, and it is free to track).
    std::size_t PeakBytes = 0;
};

//! Lua's allocator contract, from lua.h and lstate.c:
//!
//!   * `block == nullptr`                     -> allocate `newSize`; **`oldSize` is a type tag, not a size** (Lua
//!   passes
//!     `LUA_TTHREAD` for the initial block), so it must be ignored. Subtracting it under-counts every VM's memory —
//!     which is exactly the bug this function originally had.
//!   * `block != nullptr && newSize == 0`      -> free `block`, which holds `oldSize` bytes.
//!   * `block != nullptr && newSize > 0`       -> resize `block` from `oldSize` to `newSize`. On failure realloc keeps
//!     the original block alive, and Lua leaves its own accounting untouched too, so nothing changes here.
//!
//! The budget applies to grows as well as fresh allocations, because table growth (`t[#t+1] = ...` in a loop) *is* the
//! runaway pattern we are defending against — bounding only `malloc` would let it pass the cap and keep going.
//! Rejecting a grow is safe rather than leaky, and this is worth spelling out because it is easy to get wrong in both
//! directions: on failure realloc leaves the original block allocated, and luaM_realloc_ returns null **without
//! updating GCdebt**, so Lua still considers those bytes live. Both sides keep accounting for the old block, which
//! matches what we do here (nothing changes on a failed realloc), and nothing is lost.
//!
//! Nothing is logged from here: this runs inside every Lua allocation, and the logger allocates. Re-entering the
//! allocator (or taking a lock) turns memory pressure into a deadlock or a corrupted heap. Failures surface as Lua's
//! own "not enough memory" error through the protected call, which names the script responsible — something this
//! function cannot do.
void* ScriptingAllocator(void* userData, void* block, std::size_t oldSize, std::size_t newSize) noexcept
{
    auto& state = *static_cast<AllocatorState*>(userData);

    // Bytes this operation would release from the live total: nothing for a fresh allocation, `oldSize` when resizing
    // or freeing an existing block. Lua guarantees `oldSize` is exact in that case — and note it must be treated as a
    // type tag rather than a size when `block == nullptr`, which is how the initial state block is requested (lstate.c
    // passes LUA_TTHREAD as oldSize). Reading it as a size there under-counts every VM by several bytes at startup.
    const std::size_t released = (block == nullptr) ? 0u : oldSize;

    if (state.LiveBytes - released + newSize > state.LimitBytes)
    {
        // Budget exhausted. Lua raises "not enough memory", which the protected call turns into a reported error naming
        // the script. Accounting is left untouched, matching Lua, which skips its GCdebt update on failure: the old
        // block is still allocated and must stay counted.
        return nullptr;
    }

    if (newSize == 0)
    {
        std::free(block);
        state.LiveBytes -= released;
        return nullptr;
    }

    void* result = nullptr;
    if (block == nullptr)
    {
        result = std::malloc(newSize);
    }
    else
    {
        // A failed realloc leaves the original block allocated, so on failure nothing here changes and `state` keeps
        // accounting for it — which is exactly right.
        result = std::realloc(block, newSize);
    }

    if (result != nullptr)
    {
        state.LiveBytes = state.LiveBytes - released + newSize;
        if (state.LiveBytes > state.PeakBytes)
        {
            state.PeakBytes = state.LiveBytes;
        }
    }
    return result;
}

std::string ToStringOrPlaceholder(const sol::error& error)
{
    const char* what = error.what();
    return what != nullptr ? std::string(what) : std::string("<lua error with no message>");
}

}  // namespace

struct ScriptVM::Impl
{
    AllocatorState Allocator;

    //! The raw state is owned by us (created with lua_newstate, closed in the destructor).
    //!
    //! Deliberately no stored sol::state_view member. A state_view owns a managed reference to Lua's registry, so as a
    //! member it creates two lifetime traps we hit in practice: destroying it after lua_close runs luaL_unref against a
    //! freed state (a segfault inside Lua's stack handling), and copying it duplicates that ownership bookkeeping. Both
    //! disappear when the view is built where it is used — a local constructed while the state is alive cannot outlive
    //! it. It also drops an std::optional around a non-trivial member and keeps "who owns the state" answerable from
    //! one line.
    lua_State* State = nullptr;

    std::uint64_t ErrorCount = 0;
};

ScriptVM::ScriptVM()
{
    // Created before the VM so its address can be handed to Lua as the allocator's userdata and stay valid for the
    // state's whole lifetime (it is referenced from inside every allocation).
    m_Impl = std::make_unique<Impl>();

    lua_State* state = lua_newstate(ScriptingAllocator, &m_Impl->Allocator);
    if (state == nullptr)
    {
        // No VM at all: IsInitialized() reports false and every entry point degrades to a no-op with an error. A game
        // that cannot create a script VM should surface that rather than crash during startup.
        HP_LOG_ERROR("Scripting", "Failed to create the Lua state; scripting is disabled");
        return;
    }

    m_Impl->State = state;

    // Libraries are opened individually rather than with luaL_openlibs, so the default is *closed*: adding a library to
    // Lua upstream, or to this list by mistake, cannot silently grant file, process or dynamic-loading access. Scripts
    // are content, and content that can write files (io), run commands or load native modules (os, package, require)
    // has no upside worth the attack surface — anything they need is added as an explicit engine API instead.
    //
    // coroutine is included because gameplay code uses it for multi-frame sequences; string/table/math/utf8 are pure
    // computation and safe. debug is deliberately absent: it can reach into other functions' locals and frames, which
    // breaks the isolation between scripts (and a hot-reload tool that needs it should be a separate build option).
    static const luaL_Reg libraries[] = {
        {"base", luaopen_base},     {"coroutine", luaopen_coroutine}, {"table", luaopen_table}, {"math", luaopen_math},
        {"string", luaopen_string}, {"utf8", luaopen_utf8},           {nullptr, nullptr},
    };

    for (const luaL_Reg& library : libraries)
    {
        // luaL_Reg arrays are sentinel-terminated and a range-for walks the whole array including that terminator, so
        // the check is required — passing a null module name to luaL_requiref segfaults inside Lua's string interning.
        if (library.name == nullptr)
        {
            break;
        }

        luaL_requiref(state, library.name, library.func, 1);
        lua_pop(state, 1);
    }

    // `base` still provides load/dofile, which let a script compile arbitrary text into new code — enough to build a
    // function whose body bypasses whatever the host intended to allow (and, with string.dump tricks, to obscure where
    // it came from). Nil them out so all code enters through ScriptVM::Run, where chunk names and limits are ours.
    for (const char* forbidden : {"load", "dofile", "require", "package"})
    {
        lua_pushnil(state);
        lua_setglobal(state, forbidden);
    }

    // No sol view is stored (see Impl::State): it is constructed where used so it can never outlive the state.

    HP_LOG_INFO("Scripting", "Lua VM ready (limit {} KiB)", m_Impl->Allocator.LimitBytes / 1024u);
}

ScriptVM::~ScriptVM()
{
    // Engine-owned context first, and strictly before lua_close. Its destructor may reach the World it was given, and
    // that world is owned by the host — which is exactly the ordering a Lua __gc finalizer cannot promise (a finalizer
    // runs whenever the collector decides, including after the world is gone). Doing it here is why the API layer hands
    // us a deleter instead of capturing state in a sol2 closure.
    if (m_EngineAPIContext != nullptr && m_DestroyEngineAPIContext != nullptr)
    {
        m_DestroyEngineAPIContext(m_EngineAPIContext);
        m_EngineAPIContext = nullptr;
    }

    if (m_Impl && m_Impl->State != nullptr)
    {
        // Closing runs no finalizers, so script-side references to engine handles cannot execute during shutdown —
        // which is exactly what we want: nothing can touch a World that may already be gone.
        lua_close(m_Impl->State);
        m_Impl->State = nullptr;
    }
}

// The move operations are written out rather than defaulted because the raw engine-API context must be cleared in the
// source object. Leaving its pointer set would destroy the same context twice — once per VM — and the second
// destruction is a use-after-free that only surfaces once both objects reach the end of their lifetimes.
ScriptVM::ScriptVM(ScriptVM&& other) noexcept
    : m_Impl(std::move(other.m_Impl)),
      m_EngineAPIContext(other.m_EngineAPIContext),
      m_DestroyEngineAPIContext(other.m_DestroyEngineAPIContext)
{
    other.m_EngineAPIContext = nullptr;
    other.m_DestroyEngineAPIContext = nullptr;
}

ScriptVM& ScriptVM::operator=(ScriptVM&& other) noexcept
{
    if (this != &other)
    {
        // Our own context is destroyed before the pointer to it is overwritten, or it would leak.
        if (m_EngineAPIContext != nullptr && m_DestroyEngineAPIContext != nullptr)
        {
            m_DestroyEngineAPIContext(m_EngineAPIContext);
        }

        m_Impl = std::move(other.m_Impl);
        m_EngineAPIContext = other.m_EngineAPIContext;
        m_DestroyEngineAPIContext = other.m_DestroyEngineAPIContext;

        other.m_EngineAPIContext = nullptr;
        other.m_DestroyEngineAPIContext = nullptr;
    }
    return *this;
}

void ScriptVM::SetEngineAPIContext(void* context, void (*destroy)(void*) noexcept) noexcept
{
    // Replacing an existing context destroys the previous one immediately rather than at some later point: re-binding
    // the API (a hot reload, a tool switching worlds) replaces the `hp` table wholesale, so the old context is already
    // unreachable and holding it alive would pin the previous World.
    if (m_EngineAPIContext != nullptr && m_DestroyEngineAPIContext != nullptr && m_EngineAPIContext != context)
    {
        m_DestroyEngineAPIContext(m_EngineAPIContext);
    }

    m_EngineAPIContext = context;
    m_DestroyEngineAPIContext = destroy;
}

bool ScriptVM::IsInitialized() const noexcept
{
    return m_Impl != nullptr && m_Impl->State != nullptr;
}

lua_State* ScriptVM::GetLuaState() noexcept
{
    return m_Impl ? m_Impl->State : nullptr;
}

const lua_State* ScriptVM::GetLuaState() const noexcept
{
    return m_Impl ? m_Impl->State : nullptr;
}

void ScriptVM::ReportError(std::string_view message)
{
    ++m_Impl->ErrorCount;

    // Reported through the engine logger rather than thrown. A script error is a content bug: the frame's remaining
    // behaviour is lost, the game keeps running, and the log names the chunk and line so it is actionable.
    // std::format-style braces: the engine logger is not printf, and a "%.*s" here would print literally (a `%` is
    // not special to std::format, so it compiles and silently loses the message).
    HP_LOG_ERROR("Scripting", "{}", message);
}

bool ScriptVM::Run(std::string_view code, std::string& outError, std::string_view chunkName)
{
    if (!IsInitialized())
    {
        outError = "scripting is not initialized";
        return false;
    }

    // Chunk name is what appears in every traceback and error message, so it must be the file the code came from —
    // "chunk" for everything turns a log full of errors into a puzzle about which of forty files produced them.
    const std::string chunk(chunkName.empty() ? std::string_view("chunk") : chunkName);
    const std::string source(code);

    // load then call, rather than View.script, so a compile failure and a runtime failure are distinguishable in the
    // message: "expected near line 12" versus "line 40: attempt to index" send a developer to very different places.
    sol::state_view view(m_Impl->State);  // Non-const: state_view::load is not a const member.
    const sol::load_result loaded = view.load(source, chunk, sol::load_mode::text);
    if (!loaded.valid())
    {
        outError = ToStringOrPlaceholder(loaded.get<sol::error>());
        ReportError(chunk + ": " + outError);
        return false;
    }

    const sol::protected_function compiled = loaded;
    const sol::protected_function_result executed = compiled();
    if (!executed.valid())
    {
        outError = ToStringOrPlaceholder(executed.get<sol::error>());
        ReportError(chunk + ": " + outError);
        return false;
    }

    outError.clear();
    return true;
}

bool ScriptVM::HasFunction(std::string_view name) const
{
    if (!IsInitialized() || name.empty())
    {
        return false;
    }

    // Done with the C API rather than sol2 here: a lookup through sol2 has to decide what to do about a key that holds
    // a non-function (a number, say), and "is it callable" is one call in C with no such question. It also costs no
    // conversions, which matters because this runs per entity per frame for the dispatch fast path.
    // The const_cast is about Lua's C API, not mutability of our own object: lua_getglobal pushes onto the VM stack,
    // which is internal scratch space and changes nothing observable through this class (no global is written). Marking
    // HasFunction non-const would push the problem out to every caller for no benefit.
    const std::string key(name);
    lua_State* state = const_cast<lua_State*>(GetLuaState());

    lua_getglobal(state, key.c_str());
    const bool callable = lua_isfunction(state, -1) != 0;
    lua_pop(state, 1);

    return callable;
}

bool ScriptVM::CallUpdate(std::string_view functionName, std::int64_t entityHandle, float deltaTimeSeconds)
{
    if (!IsInitialized())
    {
        return false;
    }

    const std::string key(functionName);
    sol::state_view view(m_Impl->State);  // Non-const: state_view's lookups are not const members.
    const sol::optional<sol::protected_function> function = view[key];
    if (!function.has_value() || !function->valid())
    {
        // Absent is normal: most entities have no script, and a script may legitimately define only some hooks. Not an
        // error, so nothing is reported — reporting here would flood the log once per entity per frame.
        return false;
    }

    const sol::protected_function_result result = (*function)(entityHandle, deltaTimeSeconds);
    if (!result.valid())
    {
        ReportError(key + ": " + ToStringOrPlaceholder(result.get<sol::error>()));
        return false;
    }

    return true;
}

std::size_t ScriptVM::GetMemoryUsageBytes() const noexcept
{
    return m_Impl ? m_Impl->Allocator.LiveBytes : 0u;
}

std::size_t ScriptVM::GetMemoryLimitBytes() const noexcept
{
    return m_Impl ? m_Impl->Allocator.LimitBytes : 0u;
}

std::uint64_t ScriptVM::GetErrorCount() const noexcept
{
    return m_Impl ? m_Impl->ErrorCount : 0u;
}

}  // namespace Hollowpoint::Scripting
