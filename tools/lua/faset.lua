---@meta
-- Language-server declarations only. Never require this file at runtime.

---@class FasetVec3
---@field x number
---@field y number
---@field z number

---@class FasetTransform
---@field position FasetVec3 Local position in metres.
---@field rotation FasetVec3 Euler XYZ rotation in radians.
---@field scale FasetVec3 Local scale multiplier.

---@class FasetInput
---@field horizontal number
---@field vertical number
---@field jump_pressed boolean One-shot Space edge.
---@field interact_pressed boolean One-shot E edge.

---@class FasetField
---@field id? string Stable field ID; must match the enclosing map key.
---@field name? string Inspector label.
---@field type 'number'|'float'|'integer'|'int'|'boolean'|'bool'|'string'|'asset_ref'|'entity_ref'|'vec2'|'vec3'|'vec4'|'color'|'array'|'object'|'any'
---@field default any
---@field min? number
---@field max? number
---@field enum? any[]
---@field units? string

---@class FasetComponent
---@field id string Persistent component ID.
---@field type string Stable component TypeId.
---@field version integer
---@field fields table<string, any>

---@class FasetEntityRecord
---@field id string Persistent scene ID.
---@field name? string
---@field parent? string
---@field components FasetComponent[]

---@class FasetEntity
local Entity = {}

---Checks session and generation. Retained handles can become invalid.
---@return boolean
function Entity:valid() end

---Returns a copy of the simulation transform.
---@return FasetTransform
function Entity:transform() end

---Returns a copy of the presentation transform.
---@return FasetTransform
function Entity:presentation() end

---Returns a copy of component configuration, not live physics state.
---@param component_type string
---@return table<string, any>
function Entity:fields(component_type) end

---@return FasetVec3 velocity Linear velocity in metres per second. Requires a rigid body.
function Entity:velocity() end

---@return boolean grounded Requires a rigid body; uses completed physics contacts.
function Entity:is_grounded() end

---Sets a non-physical object's simulation transform.
---@param transform FasetTransform
function Entity:set_transform(transform) end

---Writes display-only pose. Permitted only during late_update.
---@param transform FasetTransform
function Entity:set_presentation(transform) end

---Discontinuous pose change, including physical bodies; preserves velocity.
---@param transform FasetTransform
function Entity:teleport(transform) end

---@param velocity FasetVec3
function Entity:set_velocity(velocity) end

---@param impulse FasetVec3
function Entity:apply_impulse(impulse) end

---Queues destruction at the next fixed-tick barrier.
function Entity:destroy() end

---Queues a full component record at the next fixed-tick barrier.
---@param component FasetComponent
function Entity:add_component(component) end

---Queues removal at the next fixed-tick barrier.
---@param component_type string
function Entity:remove_component(component_type) end

---@class FasetCollision
---@field first FasetEntity
---@field second FasetEntity
---@field other FasetEntity The entity opposite this behavior's owner.
---@field began boolean True for contact begin; false for contact end.

---@class FasetBehaviorDefinition
---@field id string Stable custom component TypeId (faset.* is reserved).
---@field version? integer Positive schema version; defaults to 1.
---@field name? string Inspector label.
---@field fields table<string, FasetField>
---@field migrations? table[] Declarative authoring migrations, not Lua callbacks.

---@class FasetBehavior: FasetInstance
---@field on_start? fun(self: FasetInstance)
---@field fixed_update? fun(self: FasetInstance, delta: number)
---@field update? fun(self: FasetInstance, delta: number)
---@field late_update? fun(self: FasetInstance, delta: number)
---@field on_destroy? fun(self: FasetInstance)
---@field on_collision? fun(self: FasetInstance, event: FasetCollision)

---@class FasetInstance
---@field entity FasetEntity Opaque, generation-checked runtime handle.
---@field fields table<string, any> Per-instance configuration: defaults plus scene overrides.
---@field state table<string, any> Private mutable state, reset on restart/reload.

faset = {}

---@type userdata Explicit JSON null. Unlike nil, retains a key in a JSON object.
faset.null = nil

---Declares one behavior in an entry script. Return this table from the file.
---@param definition FasetBehaviorDefinition
---@return FasetBehavior
function faset.behavior(definition) end

---@return FasetInput
function faset.input() end

---@param persistent_id string
---@return FasetEntity? entity Nil if the scene ID is not present.
function faset.find(persistent_id) end

---Writes a bounded message to Player logs / Editor Console.
---@param ... any
function faset.log(...) end

---Queues an entity at the next fixed tick. Does not return a handle.
---@param entity FasetEntityRecord
function faset.spawn(entity) end
