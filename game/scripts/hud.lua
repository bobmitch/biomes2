-- The in-game HUD (game/ui/hud.rml): the crosshair and what E would do, the
-- hotbar, health with the foods eaten over it, stamina, the clock, the
-- message line, and the dig radius (the "dig_radius" tunable, , and . step).
--
-- Every frame it asks the engine for the player (engine.player()) and the
-- inventory, and touches an element only when what it shows has changed:
-- RmlUi lays a document out again whenever its content changes.

hud = {}

local doc
local names = {}        -- the dig radius choices, by tunable value + 1
local last = {}         -- what each element last showed, by element id
local hidden = false
local hotbar_hidden = false
local msgs = {}         -- {text, born}
local kMsgLife = 5.0
local kMsgMax = 5

local function esc(s)
	return (tostring(s):gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"))
end

-- Sets an element's content when it differs from what it showed.
local function set(id, rml)
	if last[id] == rml then return end
	last[id] = rml
	local el = doc:GetElementById(id)
	if el then el.inner_rml = rml end
end

local function set_width(id, frac)
	local v = string.format("%.1f%%", math.max(0, math.min(1, frac)) * 100)
	local key = id .. ".w"
	if last[key] == v then return end
	last[key] = v
	doc:GetElementById(id).style.width = v
end

local function set_opacity(id, a)
	local v = string.format("%.2f", a)
	local key = id .. ".o"
	if last[key] == v then return end
	last[key] = v
	doc:GetElementById(id).style.opacity = v
end

local function set_class(id, cls, on)
	local key = id .. "." .. cls
	if last[key] == on then return end
	last[key] = on
	doc:GetElementById(id):SetClass(cls, on)
end

-- "12:34", "9:05", "45 s"
function hud.duration(s)
	s = math.max(0, math.floor(s + 0.5))
	if s < 60 then return s .. " s" end
	return string.format("%d:%02d", math.floor(s / 60), s % 60)
end

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

-- The message line: anything the engine said since the last frame, the
-- newest at the bottom, each fading out over its last second.
local function update_messages()
	local now = engine.time()
	local changed = false
	for _, m in ipairs(engine.messages()) do
		msgs[#msgs + 1] = {text = m, born = now}
		changed = true
	end
	while #msgs > kMsgMax do table.remove(msgs, 1); changed = true end
	while #msgs > 0 and now - msgs[1].born > kMsgLife do table.remove(msgs, 1); changed = true end
	if changed then
		local rows = {}
		for i, m in ipairs(msgs) do
			rows[#rows + 1] = string.format('<div id="msg%d"><span>%s</span></div>', i, esc(m.text))
		end
		last["msgs"] = nil
		set("msgs", table.concat(rows))
		for i = 1, kMsgMax do last["msg" .. i .. ".o"] = nil end
	end
	for i, m in ipairs(msgs) do
		set_opacity("msg" .. i, math.min(1, kMsgLife - (now - m.born)))
	end
end

local function update_hotbar(p)
	local inv = engine.inventory()
	local rows = {}
	for i = 1, 8 do
		local s = inv[i]
		local cls = (p.equipped == i) and "slot equipped" or "slot"
		local inner = string.format('<div class="key">%d</div>', i)
		if s and s.id ~= "" then
			inner = inner .. string.format('<img src="icon:%s"/>', s.id)
			if s.count > 1 then inner = inner .. string.format('<div class="count">%d</div>', s.count) end
		end
		rows[#rows + 1] = string.format('<div class="%s">%s</div>', cls, inner)
	end
	set("hotbar", table.concat(rows))
end

local function update_vitals(p)
	-- Foods: icon, time left, and a bar for its share of its time; the
	-- border lights when it can be eaten again.
	local rows = {}
	for i, f in ipairs(p.foods) do
		rows[#rows + 1] = string.format(
			'<div class="food%s"><img src="icon:%s"/><div class="time">%s</div>' ..
			'<div class="left" style="width: %ddp;"></div></div>',
			f.can_eat and " hungry" or "", f.id, hud.duration(f.time_left), math.floor(40 * f.share + 0.5))
	end
	set("foods", table.concat(rows))

	set_width("health-fill", p.max_health > 0 and p.health / p.max_health or 0)
	set("health-text", string.format("%d / %d", math.floor(p.health + 0.5), math.floor(p.max_health + 0.5)))

	-- Stamina shows while it is in use or coming back, then fades away.
	local full = p.stamina >= p.max_stamina - 0.5
	local a = 1
	if full then a = math.max(0, 1 - (p.since_stamina - 2.0)) end
	set_opacity("stamina-box", p.walking and a or 0)
	set_width("stamina-fill", p.max_stamina > 0 and p.stamina / p.max_stamina or 0)
	set("stamina-text", string.format("%d / %d", math.floor(p.stamina + 0.5), math.floor(p.max_stamina + 0.5)))
	set_class("stamina", "empty", p.stamina <= 0.5)

	local h = math.floor(p.hour)
	local m = math.floor((p.hour - h) * 60)
	set("clock", string.format("Day %d &nbsp; %02d:%02d", p.day, h, m))
end

-- The inventory shows its own top row; the HUD's hotbar goes while it is open.
function hud.set_hotbar_hidden(h)
	hotbar_hidden = h
end

function hud.update(paused)
	if not doc then return end
	if paused ~= hidden then
		hidden = paused
		if paused then doc:Hide() else doc:Show() end
	end
	if paused then return end
	local i = math.floor(engine.get("dig_radius") + 0.5)
	set("dig", "<span>dig radius " .. esc(names[i + 1] or "?") .. " &nbsp;&nbsp; [right click] dig &nbsp; [, .] size</span>")

	local prompt = engine.prompt()
	set("prompt", prompt ~= "" and ("<span>" .. esc(prompt) .. "</span>") or "")
	update_messages()

	local p = engine.player()
	if not p then return end
	if hotbar_hidden then set("hotbar", "") else update_hotbar(p) end
	update_vitals(p)
end
