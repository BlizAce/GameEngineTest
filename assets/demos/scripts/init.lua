-- Hollowpoint demo behaviours, loaded by the runtime from `scriptDirectory` (default: ./scripts).
--
-- Loaded once at startup as `init.lua`, before scene script names are re-bound — so a scene file can carry
-- `"Script": { "scriptName": "Spin" }` and have it run without any further wiring. See
-- RuntimeApplication::InitialiseScripts for the load/bind order.
--
-- The VM is closed by default: `io`, `os`, `package` and `require` do not exist, so a behaviour cannot reach the
-- filesystem or a process. Everything from the engine arrives through the `hp` table (see Scripting/EngineAPI.h for the
-- full surface).

local TAU = math.pi * 2.0

-- Spin: turns its entity about +Y at a constant angular speed.
--
-- Local rotation and local position throughout, deliberately. hp.get_position returns WORLD position while hp.set_position
-- writes LOCAL position (each is the right default for its own use), so a "bob relative to where I am" written against the
-- world pair silently walks a parented entity toward world axes. get_local_position is the counterpart that matches what
-- set_position stores, and it is the one an authored local offset wants.
Spin = {
    speed = TAU / 6.0, -- radians/second: one revolution every six seconds
}

function Spin.on_create(entity)
    Spin.angle = Spin.angle or {}
    Spin.angle[entity] = 0.0
end

function Spin.on_update(entity, delta_time)
    local angle = (Spin.angle and Spin.angle[entity] or 0.0) + Spin.speed * delta_time
    Spin.angle[entity] = angle

    -- Yaw only: a quaternion from axis (0,1,0) is exactly what set_rotation_euler with pitch=roll=0 produces, and naming
    -- the angles reads better at the call site than building the four components by hand.
    return hp.set_rotation_euler(entity, angle, 0.0, 0.0)
end

function Spin.on_destroy(entity)
    if Spin.angle then
        Spin.angle[entity] = nil
    end
end

-- Bob: oscillates the entity vertically about its authored height. Composable with Spin on one entity — the two behaviours
-- touch different fields of the same LocalTransform, which is why neither reads the other's data.
Bob = {
    amplitude = 0.15,
    speed = 2.0,
}

function Bob.on_create(entity)
    Bob.origin = Bob.origin or {}
    local x, y, z, ok = hp.get_local_position(entity)
    if ok then
        Bob.origin[entity] = { x = x, y = y, z = z }
    end
end

function Bob.on_update(entity, delta_time)
    local home = Bob.origin[entity]
    if home == nil then
        return false -- no LocalTransform when it was created: nothing sensible to oscillate about
    end

    home.phase = (home.phase or 0.0) + Bob.speed * delta_time
    return hp.set_position(entity, home.x, home.y + Bob.amplitude * math.sin(home.phase), home.z)
end

function Bob.on_destroy(entity)
    if Bob.origin then
        Bob.origin[entity] = nil
    end
end

-- Reported rather than printed: the log is where a missing or misnamed behaviour becomes visible during a run, and one line
-- confirming the bootstrap loaded distinguishes "scripts did not load" from "no entity references them".
hp.log_info("demo init.lua loaded")
