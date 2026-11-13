-- A health charger made in Lua (an example of a scripted entity).
-- Hold +use on it: 1 health every 0.1 s, up to 100. It holds 75; a minute after running dry it's full again.
-- Spawn one in front of you:  lua_run SpawnHealthCharger()
-- In a map: an entity with classname lua_healthcharger (key "charge" sets how much it holds).

ENT.PrintName = "Health charger (Lua)"
ENT.Model = "models/w_medkit.mdl"
ENT.MaxCharge = 75

function ENT:Initialize()
	util.PrecacheModel(self.Model)
	self:SetModel(self.Model)
	self:SetSolid(SOLID_BBOX)
	self:SetSize(Vector(-10, -10, 0), Vector(10, 10, 12))
	self:SetUseType(CONTINUOUS_USE)
	self.Charge = self.Charge or self.MaxCharge
	self.NextHeal = 0
	self:NextThink(CurTime() + 0.1)
end

function ENT:KeyValue(key, value)
	if key == "charge" then
		self.Charge = tonumber(value) or self.MaxCharge
	end
end

-- times are ServerTime(): they stay right in a saved game
function ENT:Use(activator)
	if not IsValid(activator) or not activator:IsPlayer() then return end
	local now = ServerTime()
	if now < self.NextHeal then return end
	self.NextHeal = now + 0.1
	if self.Charge <= 0 or activator:Health() >= 100 then
		if now > (self.NextDeny or 0) then
			self:EmitSound("items/medshotno1.wav")
			self.NextDeny = now + 1
		end
		return
	end
	if not self.Charging then
		self:EmitSound("items/medshot4.wav")
		self.Charging = true
	end
	activator:SetHealth(activator:Health() + 1)
	self.Charge = self.Charge - 1
	self.LastUse = now
	if self.Charge <= 0 then
		self.EmptyAt = now
	end
end

function ENT:Think()
	local now = ServerTime()
	if self.Charging and now - (self.LastUse or 0) > 0.3 then
		self.Charging = false
	end
	if self.Charge <= 0 and self.EmptyAt and now - self.EmptyAt > 60 then
		self.Charge = self.MaxCharge
		self.EmptyAt = nil
		self:EmitSound("items/medshot4.wav")
	end
	self:NextThink(CurTime() + 0.1)
end

-- on the floor, a little in front of the player
function SpawnHealthCharger()
	local ply = LocalPlayer()
	if not IsValid(ply) then return end
	local fwd = ply:GetAimVector()
	fwd.z = 0
	fwd:Normalize()
	local start = ply:EyePos()
	local ahead = util.TraceLine{ start = start, endpos = start + fwd * 48, filter = ply }
	local spot = ahead.HitPos - fwd * 12
	local down = util.TraceLine{ start = spot, endpos = spot - Vector(0, 0, 256), filter = ply }
	local e = ents.Create("lua_healthcharger")
	e:SetPos(down.HitPos)
	e:Spawn()
	return e
end
