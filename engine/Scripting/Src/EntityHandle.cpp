#include "Hollowpoint/Scripting/EntityHandle.h"

namespace Hollowpoint::Scripting
{

using ECS::Entity;

namespace
{

// Slot occupies the low 32 bits, generation the high 32. Both fields are already 32-bit in Entity, so nothing is lost
// and the layout is obvious when reading a crash dump or a debug print — which matters more than any cleverer packing
// would, since script handles show up in logs.
constexpr std::uint64_t SlotMask = 0xFFFFFFFFull;

[[nodiscard]] constexpr std::uint64_t Pack(std::uint32_t slot, std::uint32_t generation) noexcept
{
    return (static_cast<std::uint64_t>(generation) << 32u) | static_cast<std::uint64_t>(slot);
}

}  // namespace

std::int64_t EncodeEntity(const Entity& entity) noexcept
{
    if (entity.IsNull())
    {
        return 0;
    }

    // Handles are `packed + 1`, which is what reserves 0 for "no entity" without aliasing anything.
    //
    // The obvious alternative — encode normally and remap the single colliding value (slot 0, generation 0) to 1 — is
    // wrong, and was what this function first did: 1 is the legitimate encoding of entity (slot 1, generation 0), so a
    // script holding the first entity created in a fresh world would silently address the *second* one. A collision
    // remap has to land on an unused value, and there isn't one to spare.
    //
    // Offsetting by one is total instead: packed == UINT64_MAX is unreachable because its low word (the slot) would be
    // all-ones, which is ECS's NullSlot and never a live entity, so no live entity can encode to 0 after the offset.
    const std::uint64_t packed = Pack(entity.Slot, entity.Generation);
    return static_cast<std::int64_t>(packed + 1ull);
}

bool TryDecodeEntity(std::int64_t handle, Entity& outEntity) noexcept
{
    if (handle == 0)
    {
        outEntity = Entity();
        return false;
    }

    // Inverse of the +1 in EncodeEntity. Unsigned arithmetic, so a negative handle wraps to a large pattern rather than
    // being undefined behaviour; what matters is that the slot check below still rejects the ones naming no entity.
    const std::uint64_t packed = static_cast<std::uint64_t>(handle) - 1ull;
    const std::uint32_t slot = static_cast<std::uint32_t>(packed & SlotMask);
    const std::uint32_t generation = static_cast<std::uint32_t>(packed >> 32u);

    // Slot 0xFFFFFFFF is ECS's InvalidIndex/NullSlot: a handle whose low word is all ones did not come from a live
    // entity, and handing it to the world would index out of bounds.
    if (slot == Entity::NullSlot)
    {
        outEntity = Entity();
        return false;
    }

    outEntity = Entity(slot, generation);
    return true;
}

Entity DecodeEntity(std::int64_t handle) noexcept
{
    Entity entity;
    (void)TryDecodeEntity(handle, entity);  // A failure leaves `entity` null, which is the whole point here.
    return entity;
}

}  // namespace Hollowpoint::Scripting
