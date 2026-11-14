-- A Lua weapon lying in the world. Every Lua weapon gets a scripted entity of its own class built on this
-- one, so a weapon can be put in a map (an entity with the weapon's classname) or spawned from Lua:
--   local e = ents.Create("weapon_lua_pistol")  e:SetPos(pos)  e:Spawn()
-- Walking over it gives the weapon. A player who has it already gets its ammo; if none of that fits, it
-- stays where it is. A weapon that wants a different pickup ships lua/entities/<its class>.lua.

ENT.PrintName = "Lua weapon pickup"
ENT.FallbackModel = "models/w_9mmhandgun.mdl"   -- for a weapon without SWEP.WorldModel
ENT.PickupSound = "items/gunpickup2.wav"

function ENT:Initialize()
	local swep = weapons.Get(self.WeaponClass or "")
	local model = swep and swep.WorldModel
	if not model or model == "" then model = self.FallbackModel end
	util.PrecacheModel(model)
	self:SetModel(model)
	self:SetSolid(SOLID_TRIGGER)      -- touched, not bumped into
	self:SetMoveType(MOVETYPE_TOSS)   -- drops to the floor
	self:SetSize(Vector(-16, -16, 0), Vector(16, 16, 16))
end

function ENT:Touch(other)
	if self.Taken or not other:IsPlayer() or not other:Alive() then return end
	local class = self.WeaponClass
	local had = other:GetWeapon(class)
	local before = had and had:Ammo1() or 0
	local weapon = other:Give(class)
	if not weapon then return end
	if had and weapon:Ammo1() == before then return end   -- nothing gained: leave it for later
	other:EmitSound(self.PickupSound)
	self.Taken = true
	self:Remove()
end
