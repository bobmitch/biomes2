-- The player's settings: the display and the "Graphics" tunables, kept in
-- build/settings.lua as a Lua table (written here, read back with loadfile).
-- Under build/ like the saves: per machine, never committed.

local settings = {}
local PATH = "build/settings.lua"

function settings.load()
	local chunk = loadfile(PATH, "t", {})   -- an empty environment: data only
	if not chunk then return nil end
	local ok, t = pcall(chunk)
	if ok and type(t) == "table" then return t end
	engine.log("settings: ignoring an unreadable " .. PATH)
	return nil
end

local function serialise(v, indent)
	if type(v) == "table" then
		local keys = {}
		for k in pairs(v) do keys[#keys + 1] = k end
		table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
		local inner = indent .. "\t"
		local lines = {}
		for _, k in ipairs(keys) do
			local key = type(k) == "string" and k:match("^[%a_][%w_]*$") and k or "[" .. string.format("%q", k) .. "]"
			lines[#lines + 1] = inner .. key .. " = " .. serialise(v[k], inner) .. ","
		end
		return "{\n" .. table.concat(lines, "\n") .. "\n" .. indent .. "}"
	elseif type(v) == "string" then
		return string.format("%q", v)
	end
	return tostring(v)
end

function settings.save(t)
	local f, err = io.open(PATH, "w")
	if not f then
		engine.log("settings: cannot write " .. PATH .. ": " .. tostring(err))
		return false
	end
	f:write("-- Written by game/scripts/settings.lua.\nreturn " .. serialise(t, "") .. "\n")
	f:close()
	return true
end

-- What there is to save now.
function settings.capture(widgets)
	local d = engine.display()
	local t = {display = {width = d.width, height = d.height, mode = d.mode, vsync = d.vsync}, graphics = {}}
	for _, tn in ipairs(widgets.group("Graphics")) do t.graphics[tn.id] = engine.get(tn.id) end
	return t
end

-- Put saved settings back. The display only if the command line did not
-- choose one (--res), which must win: it is how a cost is measured at a size.
function settings.apply(t)
	if not t then return end
	if type(t.graphics) == "table" then
		for id, v in pairs(t.graphics) do
			if type(v) == "number" then engine.set(id, v) end
		end
	end
	if type(t.display) == "table" and not engine.display().forced then
		engine.apply_display(t.display)
	end
end

return settings
