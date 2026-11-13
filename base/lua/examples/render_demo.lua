-- render_demo.lua: drawing in the world from Lua. Run with lua_openscript examples/render_demo.lua on a map.
-- Boxes round every monster, a line to where you aim, a glowing sprite, a spinning model and name tags.

local glow = Material("sprites/glow01.spr")
local spinner

hook.Add("PostDrawOpaque", "render_demo", function()
	for _, e in ipairs(ents.GetAll()) do
		if e:IsNPC() and e:Alive() then
			render.DrawWireframeBox(e:GetPos(), Angle(0, 0, 0), e:OBBMins(), e:OBBMaxs(), Color(80, 255, 80), false)
		end
	end
	local pl = LocalPlayer()
	if IsValid(pl) then
		local tr = util.TraceLine{ start = pl:EyePos(), endpos = pl:EyePos() + pl:GetAimVector() * 4096, filter = pl }
		render.DrawWireframeSphere(tr.HitPos, 6, 8, Color(255, 200, 0))
	end
end)

hook.Add("PostDrawTranslucent", "render_demo", function()
	if IsValid(spinner) then
		render.SetMaterial(glow)
		render.DrawSprite(spinner:GetPos() + Vector(0, 0, 40), 48, 48, Color(120, 180, 255))
		render.DrawBox(spinner:GetPos(), Angle(0, 0, 0), Vector(-24, -24, -2), Vector(24, 24, 0), Color(0, 120, 255, 80))
	end
end)

hook.Add("DrawOverlay", "render_demo", function()
	for _, e in ipairs(ents.GetAll()) do
		if e:IsNPC() and e:Alive() then
			local s = (e:GetPos() + Vector(0, 0, e:OBBMaxs().z + 8)):ToScreen()
			if s.visible then
				draw.SimpleText(e:GetClass(), "default", s.x, s.y, Color(255, 255, 255))
			end
		end
	end
end)

-- a model only Lua knows about, turning in front of the player
function render_demo_spawn(model)
	local pl = LocalPlayer()
	if IsValid(spinner) then spinner:Remove() end
	spinner = ClientsideModel(model or "models/w_9mmar.mdl")
	if not spinner then return end
	local fwd = pl:GetAimVector()
	fwd.z = 0
	local spot = pl:GetPos() + fwd:GetNormalized() * 100
	local floor = util.TraceLine{ start = spot, endpos = spot - Vector(0, 0, 200), filter = pl }
	spinner:SetPos(floor.HitPos)
end

hook.Add("Think", "render_demo", function()
	if IsValid(spinner) then spinner:SetAngles(Angle(0, (RealTime() * 90) % 360, 0)) end
end)
