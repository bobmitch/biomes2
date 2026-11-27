-- The mimic (game/data/creatures.json): the player's own shape, ashen.
-- docs/adr-007-creatures-perception-navigation.md, decision 6.
--
--   IDLE      wanders near home, a short walk every so often
--   ALERT     HEARD the player (within its 20 m, loud enough): stops, turns
--             to the sound and listens; nothing more for a few seconds and
--             it goes back to idling
--   APPROACH  SEES the player (its 45 degree cone, 50 m x their visibility x
--             the light): walks towards them
--   CHASE     sees them within 40 m: runs
--   STARE     reaches them (2.2 m): stops and stares (no combat yet); they
--             step 3.5 m off and it comes on again
--   SEARCH    lost sight of them for LOSE_AFTER seconds (a chase keeps on
--             to where it last saw them meanwhile: a pursuer weaving round
--             trees has them out of its 45 degrees every few steps): walks
--             to where it last saw them
--   LOOK      there: turns about for a few seconds
--   RETURN    walks home, then idles
--
-- The engine calls on_see every tick it sees the player, on_lose the tick it
-- stops, on_hear every tick it hears them unseen, then on_think. The state is
-- an integer saved with the creature (so is its mark), so a chase frozen
-- with its zone resumes.

local IDLE, ALERT, APPROACH, CHASE, STARE, SEARCH, LOOK, RETURN = 0, 1, 2, 3, 4, 5, 6, 7

local RUN_WITHIN = 40.0    -- seen this near: run
local STARE_AT = 2.2       -- stop this near
local STARE_UNTIL = 3.5    -- ... and come on again past this
local ALERT_HOLD = 4.0     -- seconds listening after the last sound
local LOOK_FOR = 6.0       -- seconds looking about where it lost them
local LOOK_EVERY = 1.5
local WANDER = 12.0        -- metres from home
local LOSE_AFTER = 3.0     -- seconds unseen before a chase becomes a search

-- Per creature, this session only (a freeze forgets them, which only makes
-- it listen or idle afresh): when ALERT last heard something, when IDLE
-- next wanders, when LOOK next turns.
local heard, wander_at, look_at, lost_at = {}, {}, {}, {}

local M = {}

local function chasing(s)
	return s == APPROACH or s == CHASE or s == STARE
end

function M.on_wake(c)
	-- Nothing to restore: state, mark and goal were saved with it.
end

function M.on_see(c, ev)
	lost_at[c:id()] = nil
	c:set_mark(ev.x, ev.y, ev.z)
	if c:state() == STARE and ev.dist < STARE_UNTIL then
		c:face(ev.x, ev.z)
		return
	end
	if ev.dist <= STARE_AT then
		c:stop()
		c:set_state(STARE)
		c:face(ev.x, ev.z)
	elseif ev.dist <= RUN_WITHIN then
		c:set_state(CHASE)
		c:move_to(ev.x, ev.y, ev.z, "run")
	else
		c:set_state(APPROACH)
		c:move_to(ev.x, ev.y, ev.z, "walk")
	end
end

function M.on_lose(c, ev)
	if chasing(c:state()) then
		-- Keep on to where they were; on_think calls it a search if they
		-- stay unseen.
		lost_at[c:id()] = c:state_time()
		if c:state() == STARE then
			c:set_state(SEARCH)
			c:move_to(ev.x, ev.y, ev.z, "walk")
		end
	end
end

function M.on_hear(c, ev)
	if chasing(c:state()) then return end
	-- Stop and look where it came from. Hearing again keeps it listening.
	if c:state() ~= ALERT then
		c:stop()
		c:set_state(ALERT)
	end
	c:set_mark(ev.x, ev.y, ev.z)
	c:face(ev.x, ev.z)
	heard[c:id()] = c:state_time()
end

function M.on_think(c, dt)
	local id, s, t = c:id(), c:state(), c:state_time()
	if s == IDLE then
		if c:moving() then
			wander_at[id] = nil
		else
			wander_at[id] = wander_at[id] or (t + 4.0 + 8.0 * c:random())
			if t >= wander_at[id] then
				wander_at[id] = nil
				local x, y, z = c:random_point(WANDER)
				if x then c:move_to(x, y, z, "walk") end
			end
		end
	elseif s == ALERT then
		if t - (heard[id] or 0.0) > ALERT_HOLD then
			heard[id] = nil
			c:set_state(IDLE)
		end
	elseif s == APPROACH or s == CHASE then
		-- on_see steers it while it sees. Unseen too long, or there and
		-- not seeing them: a search.
		local lost = lost_at[id]
		if lost and (t - lost > LOSE_AFTER or not c:moving()) then
			lost_at[id] = nil
			c:set_state(SEARCH)
			local x, y, z = c:mark()
			c:move_to(x, y, z, "walk")
		elseif not lost and not c:moving() then
			c:set_state(SEARCH)
		end
	elseif s == SEARCH then
		if not c:moving() then
			c:set_state(LOOK)
			look_at[id] = 0.0
		end
	elseif s == LOOK then
		if t >= (look_at[id] or 0.0) then
			look_at[id] = t + LOOK_EVERY
			local x, _, z = c:pos()
			local a = c:random() * 6.2831853
			c:face(x + math.sin(a), z + math.cos(a))
		end
		if t > LOOK_FOR then
			look_at[id] = nil
			c:set_state(RETURN)
			local x, y, z = c:home()
			c:move_to(x, y, z, "walk")
		end
	elseif s == RETURN then
		if not c:moving() then c:set_state(IDLE) end
	end
end

return M
