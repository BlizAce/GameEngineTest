#include "Hollowpoint/Core/Format.h"
#include "Hollowpoint/ECS/Entity.h"

namespace Hollowpoint::ECS
{

std::string Entity::ToString() const
{
    // Used by the editor's entity picker, by assertion messages and by script error traces. The generation is
    // always printed because two handles sharing a slot but not a generation are entirely different entities:
    // a log line that omitted it would make a stale-handle bug look impossible.
    if (IsNull())
    {
        return "Entity(null)";
    }

    return Core::Format("Entity(slot={}, gen={})", Slot, Generation);
}

}  // namespace Hollowpoint::ECS
