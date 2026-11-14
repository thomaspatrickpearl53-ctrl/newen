-- A physics prop: a model that falls, tumbles, stacks and gets knocked about.
-- Spawn one:  lua_run physics.SpawnProp("hl2:models/props_junk/wooden_box01a.mdl", pos)
-- In a map: an entity with classname prop_physics and the keys "model", "mass" (kilograms; default: the
-- model's own) and "health" (it breaks when that much damage is done; default: it can't break).
-- Carrying: press use (E) on a prop light enough to pick it up; use again puts it down; attack throws it.

ENT.PrintName = "Physics prop"
ENT.PushPerDamage = 250     -- how hard a point of damage pushes (kilogram inches per second)
ENT.Unbreakable = 100000
ENT.CarryMass = 60          -- the heaviest prop a player can pick up (kilograms)
ENT.ThrowSpeed = 700        -- how fast a light prop leaves the hands; heavier ones leave slower

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
	self:SetUseType(SIMPLE_USE)
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
	-- an explosion has thrown it already, the right way
	local blast = phys:IsValid() and ServerTime() - phys:LastBlastTime() < 0.05
	if phys:IsValid() and IsValid(attacker) and not blast then
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

-- ---------------------------------------------------------------- carrying

local carried = {}          -- player's entity number -> the prop in their hands
local noattack = {}         -- player's entity number -> ServerTime until which attack is held back (after a throw)

local function Drop(ply)
	local prop = carried[ply:EntIndex()]
	carried[ply:EntIndex()] = nil
	if IsValid(prop) then prop.Carrier = nil end
	return prop
end

function ENT:Use(activator)
	if not IsValid(activator) or not activator:IsPlayer() then return end
	local n = activator:EntIndex()
	if carried[n] == self then
		Drop(activator)
		return
	end
	local phys = self:GetPhysicsObject()
	if carried[n] or self.Carrier or not phys:IsValid() or not phys:IsMotionEnabled() then return end
	if phys:GetMass() > self.CarryMass then return end
	carried[n] = self
	self.Carrier = n
	phys:Wake()
end

function ENT:OnRemove()
	if self.Carrier then carried[self.Carrier] = nil end
end

-- where a carried prop is held: in front of the eyes, far enough out that its box clears the player's
local function HoldPoint(ply, prop)
	local aim = ply:GetAimVector()
	if aim.z < -0.5 then           -- not under the player's own feet
		aim.z = -0.5
		aim:Normalize()
	end
	local size = prop:OBBMaxs() - prop:OBBMins()
	return ply:EyePos() + aim * (36 + math.max(size.x, size.y) * 0.6)
end

hook.Add("Tick", "prop_physics_carry", function()
	for n, prop in pairs(carried) do
		local ply = Entity(n)
		local phys = IsValid(prop) and prop:GetPhysicsObject()
		if not IsValid(ply) or not ply:Alive() or not phys or not phys:IsValid() then
			carried[n] = nil
			if IsValid(prop) then prop.Carrier = nil end
		else
			-- steered to the hold point by its speed, so it still bumps into things on the way
			local pull = (HoldPoint(ply, prop) - phys:GetMassCenter()) * 15
			if pull:Length() > 200 * 15 then
				Drop(ply)              -- something is holding it back (a wall between, a doorway)
			else
				if pull:Length() > 400 then pull = pull:GetNormalized() * 400 end
				phys:SetVelocity(pull + ply:GetVelocity())
				phys:SetAngleVelocity(Vector(0, 0, 0))
			end
		end
	end
end)

-- use anywhere else also lets go
hook.Add("KeyPress", "prop_physics_carry", function(ply, key)
	if key == IN_USE and carried[ply:EntIndex()] then Drop(ply) end
end)

-- attack throws what is carried instead of firing
hook.Add("StartCommand", "prop_physics_carry", function(ply, cmd)
	local n = ply:EntIndex()
	local attack = cmd.buttons & IN_ATTACK ~= 0
	if carried[n] and attack then
		local prop = Drop(ply)
		local phys = IsValid(prop) and prop:GetPhysicsObject()
		if phys and phys:IsValid() then
			local speed = math.min(prop.ThrowSpeed, prop.ThrowSpeed * 30 / phys:GetMass())
			phys:SetVelocity(ply:GetAimVector() * speed + ply:GetVelocity())
		end
		noattack[n] = ServerTime() + 0.3
	end
	if noattack[n] then
		if attack then
			cmd.buttons = cmd.buttons & ~IN_ATTACK      -- until the button is let go
			noattack[n] = math.max(noattack[n], ServerTime() + 0.05)
		elseif ServerTime() > noattack[n] then
			noattack[n] = nil
		end
	end
end)

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
