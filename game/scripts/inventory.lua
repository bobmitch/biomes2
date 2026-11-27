-- The inventory (game/ui/inventory.rml): Tab opens and closes it, the game
-- keeps running behind it, and the mouse is free while it is open.
--
-- A click on a slot takes its stack (the slot lights); a click on another
-- slot puts it there -- onto a stack of the same item, or swapping. Shift +
-- click splits the taken stack in half into an empty slot. A right click
-- uses a slot: eats a food, equips or puts away a tool. The rules are the
-- engine's (engine/player/survival.h); this only says which slots.
--
-- Global, because the document's inline handlers run in the global
-- environment.

inventory = {}

local doc
local open = false
local held = nil        -- the slot taken, 1..32
local hover = nil
local shown = {}        -- each slot's last content
local last_info

local function esc(s)
	return (tostring(s):gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"))
end

function inventory.init(ctx)
	doc = ctx:LoadDocument("game/ui/inventory.rml")
	if not doc then
		engine.log("inventory: could not load game/ui/inventory.rml")
		return false
	end
	local rows = {}
	for i = 1, 32 do
		rows[#rows + 1] = string.format(
			'<div class="islot%s" id="is%d" onmousedown="inventory.down(event, %d)" onmouseover="inventory.hover(%d)"></div>',
			i <= 8 and " hot" or "", i, i, i)
		if i == 8 then rows[#rows + 1] = '<div id="gap"></div>' end
	end
	doc:GetElementById("grid").inner_rml = table.concat(rows)
	return true
end

function inventory.is_open() return open end

function inventory.open()
	if open or not doc then return end
	open = true
	held = nil
	shown = {}
	last_info = nil
	engine.capture_mouse(false)
	if hud then hud.set_hotbar_hidden(true) end
	inventory.refresh()
	doc:Show()
end

function inventory.close(recapture)
	if not open then return end
	open = false
	held = nil
	doc:Hide()
	if hud then hud.set_hotbar_hidden(false) end
	if recapture ~= false then engine.capture_mouse(true) end
end

function inventory.toggle()
	if open then inventory.close() else inventory.open() end
end

local function is_shift(event)
	local s = event and event.parameters and event.parameters["shift_key"]
	return s == true or s == 1 or s == "1"
end

function inventory.down(event, i)
	local b = event and event.parameters and tonumber(event.parameters["button"]) or 0
	if b == 1 then
		held = nil
		engine.use_slot(i)
	elseif held == nil then
		local s = engine.inventory()[i]
		if s and s.id ~= "" then held = i end
	elseif held == i then
		held = nil
	else
		if is_shift(event) then
			engine.split_slot(held, i)
		else
			engine.move_slot(held, i)
		end
		held = nil
	end
	inventory.refresh()
end

function inventory.hover(i)
	hover = i
end

local function info_rml(s)
	if not s or s.id == "" then return "" end
	local it = engine.item(s.id)
	if not it then return "" end
	local r = {"<h2>" .. esc(it.name) .. "</h2>"}
	if it.description ~= "" then r[#r + 1] = "<p>" .. esc(it.description) .. "</p>" end
	if it.kind == "food" then
		r[#r + 1] = string.format('<div class="stat">Health <span>+%d</span></div>', it.health)
		r[#r + 1] = string.format('<div class="stat">Stamina <span>+%d</span></div>', it.stamina)
		r[#r + 1] = string.format('<div class="stat">Healing <span>%g</span> / tick</div>', it.regen)
		r[#r + 1] = string.format('<div class="stat">Lasts <span>%s</span></div>', hud.duration(it.duration))
		r[#r + 1] = '<p>Right click to eat.</p>'
	elseif it.kind == "tool" then
		r[#r + 1] = '<p>Right click (or its number on the hotbar) to equip.</p>'
	end
	if it.stack > 1 then r[#r + 1] = string.format('<div class="stat">Stack <span>%d / %d</span></div>', s.count, it.stack) end
	return table.concat(r)
end

function inventory.refresh()
	if not doc then return end
	local inv = engine.inventory()
	local p = engine.player()
	local eq = p and p.equipped or 0
	for i = 1, 32 do
		local s = inv[i]
		local inner = ""
		if i <= 8 then inner = string.format('<div class="key">%d</div>', i) end
		if s and s.id ~= "" then
			inner = inner .. string.format('<img src="icon:%s"/>', s.id)
			if s.count > 1 then inner = inner .. string.format('<div class="count">%d</div>', s.count) end
		end
		local key = inner .. (eq == i and "e" or "") .. (held == i and "h" or "")
		if shown[i] ~= key then
			shown[i] = key
			local el = doc:GetElementById("is" .. i)
			el.inner_rml = inner
			el:SetClass("equipped", eq == i)
			el:SetClass("held", held == i)
		end
	end
	local show = inv[held or hover or 0]
	local rml = info_rml(show)
	if rml ~= last_info then
		last_info = rml
		doc:GetElementById("info").inner_rml = rml ~= "" and rml or '<p>Nothing here.</p>'
	end
end

function inventory.update()
	if not open then return end
	-- A click on no element went to the game and captured the mouse: the
	-- player went back to the world, so the inventory goes.
	if engine.captured() then inventory.close(false) return end
	inventory.refresh()
end

return inventory
