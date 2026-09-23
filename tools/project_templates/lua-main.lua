-- Faset starter gameplay. Edit this file and use Refresh Lua or development reload;
-- the native C++ engine does not need to recompile for a Lua-only edit.
local Player = faset.behavior {
    id = "starter.player",
    version = 1,
    name = "Player",
    fields = {
        speed = { name = "Move speed", type = "number", default = 4, min = 0 },
        jump_speed = { name = "Jump speed", type = "number", default = 5, min = 0 }
    }
}

function Player:fixed_update(delta)
    local velocity = self.entity:velocity()
    velocity.x = faset.input().horizontal * self.fields.speed
    if faset.input().jump_pressed and self.entity:is_grounded() then
        velocity.y = self.fields.jump_speed
    end
    self.entity:set_velocity(velocity)
end

return Player
