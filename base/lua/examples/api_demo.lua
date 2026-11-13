-- api_demo.lua: the smaller Lua calls (Lua API plan step 6). lua_openscript examples/api_demo.lua on a map, then
-- api_demo() runs the checks and prints what it finds; the HUD gets a font, a picture and a key log.

surface.CreateFont("api_big", { font = "Arial", size = 28, weight = 700 })
local logo = Material("sprites/glow01.spr")
local keys = {}

hook.Add("KeyPress", "api_demo", function(ply, key) table.insert(keys, "press " .. key) if #keys > 6 then table.remove(keys, 1) end end)
hook.Add("KeyRelease", "api_demo", function(ply, key) table.insert(keys, "release " .. key) if #keys > 6 then table.remove(keys, 1) end end)
hook.Add("OnNPCKilled", "api_demo", function(npc, attacker)
	print("API OnNPCKilled", npc:GetClass(), "attacker", IsValid(attacker) and attacker:GetClass() or "none")
end)

hook.Add("HUDPaint", "api_demo", function()
	draw.SimpleText("newen Lua", "api_big", 40, 40, Color(255, 200, 80))
	draw.DrawText("line one\nline two", "api_big", 40, 80, Color(200, 220, 255))
	surface.SetMaterial(logo)
	surface.SetDrawColor(120, 180, 255, 255)
	surface.DrawTexturedRect(260, 30, 64, 64)
	for i, k in ipairs(keys) do draw.SimpleText(k, nil, 40, 160 + i * 16, color_white) end
end)

function api_demo()
	local pl = LocalPlayer()
	local sci = ents.FindByClass("monster_scientist")[1]
	print("API attachment 0", sci:GetAttachment(0) and tostring(sci:GetAttachment(0).Pos) or "none")
	local head = sci:LookupBone("Bip02 Neck")
	print("API neck bone", head, tostring(sci:GetBonePosition(head)), "missing", sci:LookupBone("no such bone"))
	print("API bodygroups", sci:GetNumBodyGroups(), "head choices", sci:GetBodygroupCount(1), "now", sci:GetBodygroup(1))
	sci:SetBodygroup(1, 2)
	print("API bodygroup set", sci:GetBodygroup(1), "body", sci.body)
	sci:SetPlaybackRate(2) print("API playback", sci:GetPlaybackRate())
	local cone = ents.FindInCone(pl:EyePos(), pl:GetAimVector(), 1000, math.cos(math.rad(30)))
	print("API in cone", #cone)
	local ray = ents.FindAlongRay(pl:EyePos(), pl:EyePos() + pl:GetAimVector() * 1000)
	print("API along ray", #ray)
	pl:SetMaxSpeed(120) print("API maxspeed", pl:GetMaxSpeed())
	pl:Freeze(true) print("API frozen", pl:IsFrozen()) pl:Freeze(false)
	print("API viewmodel", pl:GetViewModel())
	pl:PrintMessage(HUD_PRINTCENTER, "Hello from Lua")
	pl:PrintMessage(HUD_PRINTTALK, "A chat line from Lua")
	file.CreateDir("api_demo/sub")
	file.Write("api_demo/sub/x.txt", "hi")
	print("API file", file.Exists("api_demo/sub/x.txt", "DATA"), file.Delete("api_demo/sub/x.txt"), file.Exists("api_demo/sub/x.txt", "DATA"))
	cvars.AddChangeCallback("fov", function(name, old, new) print("API cvar", name, old, "->", new) end, "api_demo")
	RunConsoleCommand("fov", "100")
	print("API fired", ents.FireTargets("nothing_named_this"))
	SetNoDrawTest = sci
	sci:SetNoDraw(true) print("API nodraw", sci:GetNoDraw()) sci:SetNoDraw(false)
	sci:TakeDamage(30, pl, pl, DMG_BULLET)
end

hook.Add("CalcView", "api_demo", function(ply, origin, angles, fov)
	if api_demo_thirdperson then
		return { origin = origin - angles:Forward() * 100 + Vector(0, 0, 20), angles = angles, fov = 75 }
	end
end)
