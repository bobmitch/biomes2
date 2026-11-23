-- The tuning panel (game/ui/tuning.rml): every tunable outside "Graphics",
-- one group at a time, live over the running game. Values are NOT saved --
-- once a number is settled it belongs in archetypes.json.

local widgets = require("widgets")

tuning = {}

local doc
local shown = false
local groups = {}       -- names, in order
local current
local binder
local since_refresh = 0

local function select_group(name)
	current = name
	for i, g in ipairs(groups) do
		local b = doc:GetElementById("tg-" .. i)
		if b then b:SetClass("selected", g == name) end
	end
	binder = widgets.build(doc, doc:GetElementById("body"), widgets.group(name))
end

function tuning.init(ctx)
	doc = ctx:LoadDocument("game/ui/tuning.rml")
	if not doc then
		engine.log("tuning: could not load game/ui/tuning.rml")
		return false
	end
	groups = widgets.groups("Graphics")
	local tabs = {}
	for i, g in ipairs(groups) do
		tabs[#tabs + 1] = string.format('<button class="tab" id="tg-%d">%s</button>', i, widgets.escape(g))
	end
	doc:GetElementById("groups").inner_rml = table.concat(tabs)
	for i, g in ipairs(groups) do
		doc:GetElementById("tg-" .. i):AddEventListener("click", function() select_group(g) end, false)
	end
	if #groups > 0 then select_group(groups[1]) end
	return true
end

function tuning.is_shown() return shown end

-- Show one group by name ("Fog"); false if there is none.
function tuning.select(name)
	for _, g in ipairs(groups) do
		if g == name then
			select_group(g)
			return true
		end
	end
	return false
end

function tuning.raise()
	if shown then doc:PullToFront() end
end

function tuning.toggle()
	shown = not shown
	if shown then
		binder:refresh()
		doc:Show()
		doc:PullToFront()
		-- Free the mouse so the panel can be used; a click on the world
		-- takes it back.
		engine.capture_mouse(false)
	else
		doc:Hide()
		if not engine.paused() then engine.capture_mouse(true) end
	end
end

function tuning.update(dt)
	if not shown or not binder then return end
	since_refresh = since_refresh + dt
	if since_refresh >= 0.2 then
		since_refresh = 0
		binder:refresh()
	end
end

return tuning
