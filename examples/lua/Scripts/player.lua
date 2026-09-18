local Player = faset.behavior {
    id = "example.lua_player",
    version = 1,
    name = "Lua Player",
    fields = {
        move_speed = {
            name = "Move speed", type = "number", default = 5,
            min = 0, max = 30, units = "m/s"
        },
        jump_speed = {
            name = "Jump speed", type = "number", default = 7,
            min = 0, max = 20, units = "m/s"
        }
    }
}

function Player:on_start()
    self.state.origin = self.entity:transform()
    self.state.jumps = 0
    faset.log("Lua player ready: A/D move, Space jump, E reset")
end

function Player:fixed_update(delta)
    local input = faset.input()
    if input.interact_pressed then
        self.entity:teleport(self.state.origin)
        self.entity:set_velocity { x = 0, y = 0, z = 0 }
        self.state.jumps = 0
        return
    end

    local velocity = self.entity:velocity()
    velocity.x = input.horizontal * self.fields.move_speed
    if input.jump_pressed and self.entity:is_grounded() then
        velocity.y = self.fields.jump_speed
        self.state.jumps = self.state.jumps + 1
        faset.log("Jump", self.state.jumps)
    end
    self.entity:set_velocity(velocity)

    if self.entity:transform().position.y < -10 then
        self.entity:teleport(self.state.origin)
        self.entity:set_velocity { x = 0, y = 0, z = 0 }
    end
end

return Player
