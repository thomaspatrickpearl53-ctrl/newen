-- A physics prop: a model that falls, tumbles, stacks and gets knocked about.
-- Spawn one:  lua_run physics.SpawnProp("hl2:models/props_junk/wooden_box01a.mdl", pos)
-- In a map: an entity with classname prop_physics and the keys "model", "mass" (kilograms; default: the
-- model's own) and "health" (it breaks when that much damage is done; default: it can't break).

ENT.PrintName = "Physics prop"
ENT.PushPerDamage = 250     -- how hard a point of damage pushes (kilogram inches per second)
ENT.Unbreakable = 100000

function ENT:KeyValue(key, value)
	if key == "mass" then
		self.Mass = tonumber(value)
	elseif key == "health" then
		self.BreakHealth = tonumber(value)
	end
end

function ENT:SetUp()
	local model = self:GetModel()
	if not model or model == "" then model = "models/can.mdl" end
	util.PrecacheModel(model)
	self:SetModel(model)
	self:SetSolid(SOLID_BBOX)
	self:PhysicsInit(self.Mass or 0)
	self.takedamage = 1
end

function ENT:Initialize()
	self:SetUp()
	local breaks = self.BreakHealth and self.BreakHealth > 0
	self:SetHealth(breaks and self.BreakHealth or self.Unbreakable)
end

-- a loaded save: the entity came back where it was with the speed it had; the body is made again
function ENT:OnRestore()
	local still = self:GetVelocity():Length() < 0.01 and Vector(self.avelocity):Length() < 0.01
	self:SetUp()
	if still then self:GetPhysicsObject():Sleep() end
end

-- Shots and blows push it. The game's own weapons don't say who fired, so the player it is; the push goes
-- along their aim when that points at the prop, else straight away from them.
function ENT:OnTakeDamage(info)
	local phys = self:GetPhysicsObject()
	local attacker = info:GetAttacker()
	if not IsValid(attacker) then attacker = LocalPlayer() end
	if phys:IsValid() and IsValid(attacker) then
		local from = attacker:IsPlayer() and attacker:EyePos() or attacker:WorldSpaceCenter()
		local hit = phys:GetMassCenter()
		local dir = (hit - from):GetNormalized()
		if attacker:IsPlayer() then
			local aim = attacker:GetAimVector()
			local tr = util.TraceLine{ start = from, endpos = from + aim * 4096, filter = attacker }
			if tr.Entity == self then
				dir = aim
				hit = tr.HitPos
			end
		end
		phys:ApplyForceOffset(dir * (info:GetDamage() * self.PushPerDamage), hit)
	end
	if not (self.BreakHealth and self.BreakHealth > 0) then
		self:SetHealth(self.Unbreakable)
	end
end

physics = physics or {}

-- a prop at pos (and angles), already falling
function physics.SpawnProp(model, pos, angles)
	local e = ents.Create("prop_physics")
	if not IsValid(e) then return NULL end
	e:SetModel(model)
	e:SetPos(pos)
	if angles then e:SetAngles(angles) end
	e:Spawn()
	return e
end
