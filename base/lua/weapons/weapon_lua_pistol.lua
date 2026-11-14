-- An example Lua weapon. The file's name is the weapon's class: weapon_lua_pistol.
-- Get it with "sv_cheats 1; give weapon_lua_pistol" in the console, or from Lua:
--   player.GetByID(1):Give("weapon_lua_pistol")
--
-- So far it appears in the weapon menu (slot 2, under the pistol and the .357) with its clip and ammo.
-- Choosing and firing it are the next steps of docs/SWEP_PLAN.md.

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
	self.Shots = 0   -- plain values in the weapon's own table will be saved with the game (step 4)
end
