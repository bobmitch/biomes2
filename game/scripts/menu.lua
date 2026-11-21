-- The pause menu: main, options, controls (game/ui/menu.rml).
--
-- Global, because the document's inline handlers (onclick="menu.resume()")
-- run in the global environment.

local widgets = require("widgets")
local settings = require("settings")

menu = {}

local doc
local screen = "main"
local open = false
local graphics          -- the binder over the "Graphics" tunables

-- What the controls screen lists. Kept here rather than read from C++: it is
-- presentation, and the day keys become rebindable this table becomes the
-- binding table.
local KEYS = {
	{"Esc", "Pause menu; back; resume"},
	{"F1  or  `", "Tuning panel"},
	{"Mouse", "Look (click to capture); walking, orbit the camera"},
	{"Wheel", "Walking: camera distance, in to first person; flying: speed"},
	{"W A S D", "Move"},
	{"Q / E", "Down / up (flying)"},
	{"Shift", "Sprint; fly faster"},
	{"Caps Lock", "Walk (toggle)"},
	{"Tab", "Walk (third person) / fly"},
	{"Space", "Jump; swim up"},
	{"Ctrl", "Dive"},
	{"Click", "Swing the axe: chop the tree in front (walking)"},
	{"F", "Carry a torch"},
	{"V / C", "Drop a campfire / clear them"},
	{"PgUp / PgDn", "Time of day +/- 30 min (Shift: 5 min)"},
	{"H", "Run the clock"},
	{"[  ]", "Calmer / stormier"},
	{"\\", "Automatic weather"},
	{";", "Turn the wind 45 degrees"},
	{"- / =", "Tree draw distance"},
	{", / .", "Exposure -/+ half a stop"},
	{"U", "Tonemap: AgX / ACES / raw"},
	{"F6 / F9", "Save / load edits and felled trees"},
	{"F2", "Screenshot to build/shot.bmp"},
	{"F5", "Reload shaders"},
	{"L G Y O J N B Z P", "A/B: shadows, grass, TAA, AO, contact, mist, canopy, GI, parallax"},
	{"M / T / K", "Wireframe / texture / water debug"},
	{"F3 / F4 / F7", "Clouds (Shift: rain) / ripples / reflections"},
}

local function build_controls()
	local rows = {}
	for _, k in ipairs(KEYS) do
		rows[#rows + 1] = string.format('<tr><td class="key">%s</td><td class="what">%s</td></tr>',
			widgets.escape(k[1]), widgets.escape(k[2]))
	end
	doc:GetElementById("keys").inner_rml = table.concat(rows)
end

-- The display rows are rebuilt each time the screen opens: the monitor's
-- modes and the window's size can both have changed.
local function build_display()
	local d = engine.display()
	local modes = engine.display_modes()
	local have = false
	for _, m in ipairs(modes) do
		if m.width == d.width and m.height == d.height then have = true end
	end
	if not have then table.insert(modes, 1, {width = d.width, height = d.height}) end

	local res = {}
	for _, m in ipairs(modes) do
		local v = m.width .. "x" .. m.height
		res[#res + 1] = string.format('<option value="%s"%s>%d x %d</option>', v,
			(m.width == d.width and m.height == d.height) and ' selected=""' or "", m.width, m.height)
	end
	local function opt(v, label, cur)
		return string.format('<option value="%s"%s>%s</option>', v, v == cur and ' selected=""' or "", label)
	end
	doc:GetElementById("opt-display").inner_rml =
		'<div class="row"><div class="label">Display mode</div><select id="opt-mode">' ..
		opt("windowed", "Windowed", d.mode) .. opt("borderless", "Borderless fullscreen", d.mode) ..
		'</select></div>' ..
		'<div class="row"><div class="label">Resolution</div><select id="opt-res">' .. table.concat(res) ..
		'</select></div>' ..
		'<div class="row toggle"><input type="checkbox" id="opt-vsync"' .. (d.vsync and ' checked=""' or "") ..
		'/><span class="label">V-sync</span></div>'
	doc:GetElementById("opt-display-now").inner_rml =
		string.format("now %d x %d, %s", d.width, d.height, d.mode)
end

function menu.apply_display()
	local mode = doc:GetElementById("opt-mode"):GetAttribute("value")
	local res = doc:GetElementById("opt-res"):GetAttribute("value") or ""
	local w, h = res:match("^(%d+)x(%d+)$")
	local d = engine.display()
	engine.apply_display{
		width = tonumber(w) or d.width,
		height = tonumber(h) or d.height,
		mode = mode or d.mode,
		vsync = doc:GetElementById("opt-vsync"):HasAttribute("checked"),
	}
	settings.save(settings.capture(widgets))
	build_display()
end

function menu.init(ctx)
	doc = ctx:LoadDocument("game/ui/menu.rml")
	if not doc then
		engine.log("menu: could not load game/ui/menu.rml")
		return false
	end
	build_controls()
	graphics = widgets.build(doc, doc:GetElementById("opt-graphics"), widgets.group("Graphics"))
	return true
end

function menu.is_open() return open end

function menu.show_screen(name)
	if screen == "options" and name ~= "options" then settings.save(settings.capture(widgets)) end
	for _, s in ipairs{"main", "options", "controls"} do
		doc:GetElementById("screen-" .. s):SetClass("hidden", s ~= name)
	end
	doc:GetElementById("panel"):SetClass("wide", name ~= "main")
	if name == "options" then
		build_display()
		graphics:refresh()
	end
	screen = name
end

function menu.open()
	if open then return end
	open = true
	engine.set_paused(true)
	engine.capture_mouse(false)
	menu.show_screen("main")
	doc:Show()
	-- The panel stays usable over the menu: time and weather can be
	-- auditioned on a frozen frame.
	if tuning then tuning.raise() end
end

function menu.resume()
	if not open then return end
	if screen ~= "main" then menu.show_screen("main") end
	open = false
	doc:Hide()
	engine.set_paused(false)
	engine.capture_mouse(true)
end

-- Esc inside the menu: back out one screen, then resume.
function menu.back()
	if screen ~= "main" then menu.show_screen("main") else menu.resume() end
end

function menu.toggle_tuning()
	if tuning then tuning.toggle() end
end

function menu.update()
	if open and screen == "options" and graphics then graphics:refresh() end
end

return menu
