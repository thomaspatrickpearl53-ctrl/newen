-- Example engine-level script: a corner watermark drawn from Lua, and a console command.
-- Turn the watermark off with: newen_watermark 0

local watermark = CreateConVar("newen_watermark", "1", FCVAR_ARCHIVE, "shows the corner watermark drawn by Lua")
local accent = Color(255, 140, 0)

hook.Add("DrawOverlay", "newen.watermark", function()
	if not watermark:GetBool() then return end
	local text = "newen " .. engine.Version() .. "  |  Lua engine hooks online  |  game: " .. engine.GetGameDir()
	local w, h = surface.GetTextSize(text)
	local x, y = 12, ScrH() - h - 12
	surface.SetDrawColor(0, 0, 0, 160)
	surface.DrawRect(x - 6, y - 4, w + 12, h + 8)
	surface.SetDrawColor(accent)
	surface.DrawRect(x - 6, y - 4, 3, h + 8)
	draw.SimpleText(text, nil, x, y, color_white)
end)

concommand.Add("newen_hello", function(ply, cmd, args, argStr)
	print("Hello from Lua! You typed: " .. (argStr ~= "" and argStr or "(nothing)"))
end, nil, "prints a greeting from Lua")

hook.Add("Initialize", "newen.welcome", function()
	print("Lua is running inside the engine. Try: lua_run 2+2   or   newen_hello world")
end)
