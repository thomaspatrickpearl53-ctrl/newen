-- newen core Lua library: hook, timer, Color, draw helpers and small utilities.
-- Loaded first, before every autorun script.

---------------------------------------------------------------- hook

hook = {}
local hooks = {}   -- hooks[event][id] = fn

function hook.Add(event, id, fn)
	assert(type(event) == "string", "hook.Add: event name must be a string")
	assert(type(fn) == "function", "hook.Add: callback must be a function")
	hooks[event] = hooks[event] or {}
	hooks[event][id] = fn
end

function hook.Remove(event, id)
	if hooks[event] then
		hooks[event][id] = nil
	end
end

function hook.GetTable()
	return hooks
end

-- Calls every callback; the first one that returns a non-nil value stops the rest.
function hook.Run(event, ...)
	local list = hooks[event]
	if not list then
		return
	end
	for id, fn in pairs(list) do
		-- an id that is a table (or object) with IsValid() is removed once it's no longer valid
		if type(id) ~= "string" and type(id) ~= "number" then
			if type(id) == "table" and id.IsValid and not id:IsValid() then
				list[id] = nil
				goto continue
			end
			local a, b, c, d, e, f = fn(id, ...)
			if a ~= nil then
				return a, b, c, d, e, f
			end
		else
			local a, b, c, d, e, f = fn(...)
			if a ~= nil then
				return a, b, c, d, e, f
			end
		end
		::continue::
	end
end

hook.Call = function(event, gm, ...) return hook.Run(event, ...) end

---------------------------------------------------------------- timer

timer = {}
local timers = {}      -- named
local simple = {}      -- anonymous one-shots

function timer.Simple(delay, fn)
	simple[#simple + 1] = { time = CurTime() + delay, fn = fn }
end

-- timer.Create(name, delay, reps, fn); reps 0 = forever
function timer.Create(name, delay, reps, fn)
	timers[name] = { delay = delay, reps = reps, left = reps, next = CurTime() + delay, fn = fn, paused = false }
end

function timer.Remove(name)  timers[name] = nil end
function timer.Exists(name)  return timers[name] ~= nil end
function timer.Pause(name)   if timers[name] then timers[name].paused = true end end
function timer.UnPause(name) if timers[name] then timers[name].paused = false end end
function timer.Adjust(name, delay, reps, fn)
	local t = timers[name]
	if not t then return false end
	t.delay = delay or t.delay
	if reps then t.reps, t.left = reps, reps end
	t.fn = fn or t.fn
	return true
end
function timer.TimeLeft(name)
	local t = timers[name]
	return t and math.max(0, t.next - CurTime()) or nil
end

local function run_timer(where, fn)
	local ok, err = xpcall(fn, debug.traceback)
	if not ok then
		print("[Lua] timer " .. where .. ": " .. tostring(err))
	end
end

hook.Add("Think", "newen.timers", function()
	local now = CurTime()
	if #simple > 0 then
		local due = {}
		for i = #simple, 1, -1 do
			if simple[i].time <= now then
				due[#due + 1] = simple[i].fn
				table.remove(simple, i)
			end
		end
		for i = #due, 1, -1 do
			run_timer("Simple", due[i])
		end
	end
	for name, t in pairs(timers) do
		if not t.paused and t.next <= now then
			t.next = now + t.delay
			if t.reps > 0 then
				t.left = t.left - 1
				if t.left <= 0 then
					timers[name] = nil
				end
			end
			run_timer(tostring(name), t.fn)
		end
	end
end)

---------------------------------------------------------------- Color

local color_mt = {}
color_mt.__index = color_mt
function color_mt.__tostring(c) return string.format("%d %d %d %d", c.r, c.g, c.b, c.a) end
function color_mt.__eq(a, b) return a.r == b.r and a.g == b.g and a.b == b.b and a.a == b.a end
function color_mt:Unpack() return self.r, self.g, self.b, self.a end

function Color(r, g, b, a)
	return setmetatable({ r = r or 255, g = g or 255, b = b or 255, a = a or 255 }, color_mt)
end

function IsColor(v) return getmetatable(v) == color_mt end

color_white = Color(255, 255, 255)
color_black = Color(0, 0, 0)
color_transparent = Color(0, 0, 0, 0)

---------------------------------------------------------------- draw

draw = {}

TEXT_ALIGN_LEFT, TEXT_ALIGN_CENTER, TEXT_ALIGN_RIGHT = 0, 1, 2
TEXT_ALIGN_TOP, TEXT_ALIGN_BOTTOM = 3, 4

-- draw.SimpleText(text, font, x, y, color, xalign, yalign); font is ignored for now
function draw.SimpleText(text, font, x, y, color, xalign, yalign)
	text = tostring(text)
	local w, h = surface.GetTextSize(text)
	if xalign == TEXT_ALIGN_CENTER then x = x - w / 2 elseif xalign == TEXT_ALIGN_RIGHT then x = x - w end
	if yalign == TEXT_ALIGN_CENTER then y = y - h / 2 elseif yalign == TEXT_ALIGN_BOTTOM then y = y - h end
	surface.SetTextColor(color or color_white)
	surface.SetTextPos(x, y)
	surface.DrawText(text)
	return w, h
end

-- Rounding is not drawn yet; the box is square.
function draw.RoundedBox(radius, x, y, w, h, color)
	surface.SetDrawColor(color or color_white)
	surface.DrawRect(x, y, w, h)
end

---------------------------------------------------------------- utilities

function PrintTable(t, indent, seen)
	indent = indent or 0
	seen = seen or {}
	seen[t] = true
	local keys = {}
	for k in pairs(t) do keys[#keys + 1] = k end
	table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
	for _, k in ipairs(keys) do
		local v = t[k]
		local pad = string.rep("\t", indent)
		if type(v) == "table" and not seen[v] then
			print(pad .. tostring(k) .. ":")
			PrintTable(v, indent + 1, seen)
		else
			print(pad .. tostring(k) .. "\t=\t" .. tostring(v))
		end
	end
end

function string.Split(s, sep)
	local out, start = {}, 1
	if sep == "" then
		for i = 1, #s do out[i] = s:sub(i, i) end
		return out
	end
	while true do
		local a, b = s:find(sep, start, true)
		if not a then
			out[#out + 1] = s:sub(start)
			return out
		end
		out[#out + 1] = s:sub(start, a - 1)
		start = b + 1
	end
end
string.Explode = function(sep, s) return string.Split(s, sep) end

function string.Trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end
function string.StartWith(s, p) return s:sub(1, #p) == p end
function string.EndsWith(s, p) return p == "" or s:sub(-#p) == p end

function table.Count(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end
function table.HasValue(t, v) for _, x in pairs(t) do if x == v then return true end end return false end
function table.Copy(t, seen)
	seen = seen or {}
	if seen[t] then return seen[t] end
	local c = {}
	seen[t] = c
	for k, v in pairs(t) do c[k] = type(v) == "table" and table.Copy(v, seen) or v end
	return setmetatable(c, getmetatable(t))
end

function math.Clamp(v, lo, hi) return v < lo and lo or (v > hi and hi or v) end
function math.Round(v, dec) local m = 10 ^ (dec or 0) return math.floor(v * m + 0.5) / m end
function math.Approach(cur, target, inc)
	inc = math.abs(inc)
	if cur < target then return math.min(cur + inc, target) end
	return math.max(cur - inc, target)
end

function isstring(v) return type(v) == "string" end
function isnumber(v) return type(v) == "number" end
function istable(v) return type(v) == "table" end
function isfunction(v) return type(v) == "function" end
function isbool(v) return type(v) == "boolean" end
