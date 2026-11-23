-- The in-game HUD (game/ui/hud.rml): the crosshair a dig aims with, and the
-- dig radius (the "dig_radius" tunable, which , and . step).

hud = {}

local doc
local names = {}        -- the dig radius choices, by tunable value + 1
local last
local hidden = false

function hud.init(ctx)
	doc = ctx:LoadDocument("game/ui/hud.rml")
	if not doc then
		engine.log("hud: could not load game/ui/hud.rml")
		return false
	end
	for _, t in ipairs(engine.tunables()) do
		if t.id == "dig_radius" and t.choices then names = t.choices end
	end
	doc:Show()
	return true
end

function hud.update(paused)
	if not doc then return end
	if paused ~= hidden then
		hidden = paused
		if paused then doc:Hide() else doc:Show() end
	end
	if paused then return end
	local i = math.floor(engine.get("dig_radius") + 0.5)
	if i ~= last then
		last = i
		doc:GetElementById("dig").inner_rml = "<span>dig radius " .. (names[i + 1] or "?") .. " &nbsp;&nbsp; [right click] dig &nbsp; [, .] size</span>"
	end
end
