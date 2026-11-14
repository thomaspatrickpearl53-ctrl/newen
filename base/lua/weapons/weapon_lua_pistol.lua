-- An example Lua weapon. The file's name is the weapon's class: weapon_lua_pistol.
-- Get it with "sv_cheats 1; give weapon_lua_pistol" in the console, or from Lua:
--   player.GetByID(1):Give("weapon_lua_pistol")
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
end

function SWEP:Reload()
	if self:DefaultReload("reload") then   -- true if a reload started; the clip fills when the animation ends
		self:EmitSound("items/9mmclip1.wav")
	end
end
