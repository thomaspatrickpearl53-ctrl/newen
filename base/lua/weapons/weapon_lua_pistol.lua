-- An example Lua weapon. The file's name is the weapon's class: weapon_lua_pistol.
--
-- Get it in the console:   sv_cheats 1; give weapon_lua_pistol
-- or from Lua:             player.GetByID(1):Give("weapon_lua_pistol")
-- or lying on the floor:   lua_run SpawnLuaPistol()
-- or in a map:             an entity with classname weapon_lua_pistol
--
-- It appears in the weapon menu (slot 2, under the pistol and the .357), fires, kicks and reloads, and
-- stays with you through level changes and saved games.

game.AddAmmoType{ name = "lua_pistol", max = 120 }

SWEP.PrintName = "Lua pistol"
SWEP.Slot      = 1      -- the menu column, counted from 0 as in Garry's Mod: this is key 2
SWEP.SlotPos   = 2      -- the place in the column; a taken place moves the weapon to the next free one

SWEP.ViewModel  = "models/v_9mmhandgun.mdl"
SWEP.WorldModel = "models/w_9mmhandgun.mdl"

SWEP.Primary.ClipSize    = 12
SWEP.Primary.DefaultClip = 36            -- 12 loaded, 24 spare
SWEP.Primary.Automatic   = false
SWEP.Primary.Ammo        = "lua_pistol"

SWEP.Secondary.Ammo = "none"

function SWEP:Initialize()
	self.Shots = 0   -- plain values in the weapon's own table are saved with the game
end

-- taken in hand; the view model's "draw" has been started
function SWEP:Deploy()
	self.IdleAt = ServerTime() + 3   -- ServerTime: the map's clock, right again after a saved game
	return true
end

-- every frame while in hand: a look around now and then
function SWEP:Think()
	if self.IdleAt and ServerTime() >= self.IdleAt then
		self:SendWeaponAnim("idle" .. math.random(1, 3))
		self.IdleAt = ServerTime() + math.random(5, 9)
	end
end

-- called while the fire button is held and the weapon is ready (once per press: Automatic is false)
function SWEP:PrimaryAttack()
	if not self:CanPrimaryAttack() then return end   -- empty: clicks, and reloads if there's spare ammo
	self:ShootBullet{ damage = 12, num = 1, spread = 0.01 }
	self:TakePrimaryAmmo(1)
	self:EmitSound("weapons/pl_gun3.wav")
	self:SendWeaponAnim(self:Clip1() > 0 and "shoot" or "shoot_empty")   -- sequences of the view model
	self.Owner:ViewPunch(Angle(-2, 0, 0))
	self:SetNextPrimaryFire(CurTime() + 0.25)
	self.Shots = self.Shots + 1
	self.IdleAt = ServerTime() + 4
end

function SWEP:Reload()
	if self:DefaultReload("reload") then   -- true if a reload started; the clip fills when the animation ends
		self:EmitSound("items/9mmclip1.wav")
		self.IdleAt = ServerTime() + 5
	end
end

-- puts the pistol on the floor a little in front of the player, to walk over
function SpawnLuaPistol()
	local ply = LocalPlayer()
	if not IsValid(ply) then return end
	local fwd = ply:GetAimVector()
	fwd.z = 0
	fwd:Normalize()
	local start = ply:EyePos()
	local ahead = util.TraceLine{ start = start, endpos = start + fwd * 96, filter = ply }
	local spot = ahead.HitPos - fwd * 24   -- clear of a wall: a pickup that starts inside one falls out of the map
	local down = util.TraceLine{ start = spot, endpos = spot - Vector(0, 0, 256), filter = ply }
	local e = ents.Create("weapon_lua_pistol")
	e:SetPos(down.HitPos + Vector(0, 0, 4))
	e:Spawn()
	return e
end
