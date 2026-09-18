local motion = require("util.motion")

local Beacon = faset.behavior {
    id = "example.lua_beacon",
    version = 1,
    name = "Lua Beacon",
    fields = {
        amplitude = { name = "Height", type = "number", default = 0.3, min = 0, max = 2 },
        frequency = { name = "Frequency", type = "number", default = 0.7, min = 0, max = 5 }
    }
}

function Beacon:on_start()
    self.state.origin_y = self.entity:transform().position.y
    self.state.elapsed = 0
end

function Beacon:update(delta)
    self.state.elapsed = self.state.elapsed + delta
    local pose = self.entity:transform()
    pose.position.y = self.state.origin_y
        + motion.bob(self.state.elapsed, self.fields.frequency, self.fields.amplitude)
    pose.rotation.z = self.state.elapsed
    self.entity:set_transform(pose)
end

return Beacon
