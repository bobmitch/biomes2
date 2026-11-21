-- Controls for engine tunables (engine/ui/script_api.h): one row per tunable,
-- wired both ways. The control sets the engine as it moves; refresh() pulls
-- the engine's value back, because keys, the automatic weather and the clock
-- move things too.

local widgets = {}

local function decimals(step)
	if step >= 1 then return 0 end
	if step >= 0.1 then return 1 end
	if step >= 0.01 then return 2 end
	return 3
end

function widgets.format(t, v)
	if t.kind == "toggle" then return "" end
	if t.kind == "choice" then return t.choices[math.floor(v + 0.5) + 1] or tostring(v) end
	local s = string.format("%." .. decimals(t.step) .. "f", v)
	if t.unit ~= "" then s = s .. " " .. t.unit end
	return s
end

function widgets.escape(s)
	return (tostring(s):gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"))
end
local esc = widgets.escape

local function row_rml(t)
	local id = "tn-" .. t.id
	if t.kind == "toggle" then
		return string.format(
			'<div class="row toggle"><input type="checkbox" id="%s"%s/><span class="label">%s</span></div>',
			id, t.value > 0.5 and ' checked=""' or "", esc(t.label))
	elseif t.kind == "choice" then
		local opts = {}
		for i, name in ipairs(t.choices) do
			opts[#opts + 1] = string.format('<option value="%d"%s>%s</option>', i - 1,
				math.floor(t.value + 0.5) == i - 1 and ' selected=""' or "", esc(name))
		end
		return string.format('<div class="row"><div class="label">%s</div><select id="%s">%s</select></div>',
			esc(t.label), id, table.concat(opts))
	end
	return string.format(
		'<div class="row"><div class="label">%s<span class="value" id="tv-%s">%s</span></div>' ..
		'<input type="range" id="%s" min="%.6g" max="%.6g" step="%.6g" value="%.6g"/></div>',
		esc(t.label), t.id, esc(widgets.format(t, t.value)), id, t.min, t.max, t.step, t.value)
end

local Binder = {}
Binder.__index = Binder

-- Fills `container` (an element of `doc`) with rows for `list` and returns a
-- binder. on_change(t, v), if given, runs after each change the user makes.
function widgets.build(doc, container, list, on_change)
	local parts = {}
	for _, t in ipairs(list) do parts[#parts + 1] = row_rml(t) end
	container.inner_rml = table.concat(parts)

	local b = setmetatable({doc = doc, list = list, shown = {}, touched = {}, suppress = false,
		on_change = on_change}, Binder)
	for _, t in ipairs(list) do
		b.shown[t.id] = t.value
		local el = doc:GetElementById("tn-" .. t.id)
		if el then
			el:AddEventListener("change", function(event)
				-- refresh() moving a control fires this too: not the user's.
				if b.suppress then return end
				local v
				if t.kind == "toggle" then
					v = el:HasAttribute("checked") and 1 or 0
				else
					v = tonumber(event.parameters["value"])
				end
				if v == nil then return end
				engine.set(t.id, v)
				b.touched[t.id] = engine.time()
				b.shown[t.id] = v
				b:update_label(t, engine.get(t.id) or v)
				if b.on_change then b.on_change(t, v) end
			end, false)
		end
	end
	return b
end

function Binder:update_label(t, v)
	local lab = self.doc:GetElementById("tv-" .. t.id)
	if not lab then return end
	local s = esc(widgets.format(t, v))
	if lab.inner_rml ~= s then lab.inner_rml = s end
end

-- Pull every value from the engine. A control the user touched in the last
-- moment is left alone, so a drag is never fought.
function Binder:refresh()
	local now = engine.time()
	for _, t in ipairs(self.list) do
		local v = engine.get(t.id)
		if v then
			self:update_label(t, v)
			local recent = self.touched[t.id] and now - self.touched[t.id] < 0.75
			local shown = self.shown[t.id]
			if not recent and (shown == nil or math.abs(v - shown) > t.step * 0.5) then
				local el = self.doc:GetElementById("tn-" .. t.id)
				if el then
					self.suppress = true
					if t.kind == "toggle" then
						if v > 0.5 then el:SetAttribute("checked", "") else el:RemoveAttribute("checked") end
					elseif t.kind == "choice" then
						el:SetAttribute("value", tostring(math.floor(v + 0.5)))
					else
						el:SetAttribute("value", string.format("%.6g", v))
					end
					self.suppress = false
				end
				self.shown[t.id] = v
			end
		end
	end
end

-- The tunables in one group, in the order C++ registered them.
function widgets.group(name)
	local out = {}
	for _, t in ipairs(engine.tunables()) do
		if t.group == name then out[#out + 1] = t end
	end
	return out
end

-- Every group name except `skip`, in first-seen order.
function widgets.groups(skip)
	local seen, out = {}, {}
	for _, t in ipairs(engine.tunables()) do
		if t.group ~= skip and not seen[t.group] then
			seen[t.group] = true
			out[#out + 1] = t.group
		end
	end
	return out
end

return widgets
