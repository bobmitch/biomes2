-- The game's script entry point, run once by engine/ui/ui.cpp after the
-- engine is up. Everything the UI does is decided here and in the modules it
-- loads; C++ only routes input and calls the `game` hooks below.
--
-- Available: the `engine` module (engine/ui/script_api.h) and RmlUi's Lua
-- API (`rmlui`, documents, elements, events).

package.path = "game/scripts/?.lua;" .. package.path

local FONTS = {
	"third_party/RmlUi/Samples/assets/LatoLatin-Regular.ttf",
	"third_party/RmlUi/Samples/assets/LatoLatin-Bold.ttf",
	"third_party/RmlUi/Samples/assets/LatoLatin-Italic.ttf",
	"third_party/RmlUi/Samples/assets/RobotoMono-Regular.ttf",
}
for _, f in ipairs(FONTS) do
	if not rmlui:LoadFontFace(f) then engine.log("could not load font " .. f) end
end

local settings = require("settings")
require("hud")
require("inventory")
require("menu")
require("tuning")

local ctx = rmlui.contexts["main"]

game = {}

-- Esc, F1, `, Tab and 1-9 reach here before anything else sees them.
-- Return true to consume the key.
function game.on_key(name)
	if name == "escape" then
		if inventory.is_open() then inventory.close()
		elseif menu.is_open() then menu.back()
		else menu.open() end
		return true
	elseif name == "f1" or name == "grave" then
		tuning.toggle()
		return true
	end
	-- The rest are play's: behind the menu they are the menu's.
	if menu.is_open() then return false end
	if name == "tab" then
		inventory.toggle()
		return true
	end
	local n = tonumber(name)
	if n and n >= 1 and n <= 8 then
		engine.use_slot(n)
		if inventory.is_open() then inventory.refresh() end
		return true
	end
	return false
end

-- Every frame, on the wall clock (it runs while paused).
function game.on_update(dt)
	tuning.update(dt)
	menu.update()
	if menu.is_open() and inventory.is_open() then inventory.close(false) end
	inventory.update()
	hud.update(menu.is_open())
end

-- First, so the menu and the panel draw over it.
hud.init(ctx)
inventory.init(ctx)
if not menu.init(ctx) then error("the pause menu did not load") end
tuning.init(ctx)
settings.apply(settings.load())
engine.log("game scripts up")
