#include <set>
#include <string>

#include "Hollowpoint/Reflection/NameInterner.h"

namespace Hollowpoint::Reflection
{

const char* InternName(std::string_view name)
{
    // std::set never relocates its elements, so the returned pointer survives every later insertion. The
    // node-based container costs an allocation per unique name, and there are only ever a few hundred of
    // them (one per reflected type, field and enum entry), all at startup.
    //
    // Intentionally leaked: descriptors hold these pointers and the editor may still read them during
    // shutdown, after static destruction would have run. The total is a few kilobytes.
    static std::set<std::string>* pool = new std::set<std::string>();

    auto existing = pool->find(std::string(name));
    if (existing != pool->end())
    {
        return existing->c_str();
    }

    const auto inserted = pool->emplace(name).first;
    return inserted->c_str();
}

}  // namespace Hollowpoint::Reflection
