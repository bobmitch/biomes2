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
--   ATTACK    reaches them (just inside its own reach): stops, faces them
--             and swings -- a windup, a blow that lands only if they are
--             still in reach and in front of it, then its recovery. They
--             step clear and it comes on again. A kind with no attack
--             stares instead (STARE), which is what this did before.
--   HURT      being struck turns it on whoever did it, wherever it was
--             looking (on_hurt)
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
local ATTACK = 8

local RUN_WITHIN = 40.0    -- seen this near: run
local STARE_AT = 2.2       -- stop this near, with no attack of its own
local REACH_PAD = 0.3      -- ... or this far inside its reach, with one
local HOLD_GIVE = 1.3      -- it holds them until they are this much further off
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
	return s == APPROACH or s == CHASE or s == STARE or s == ATTACK
end

-- Where it stops: just inside its own reach, or STARE_AT if it has none.
local function closes_to(c)
	local reach = c:attack_reach()
	if reach > 0 then return reach - REACH_PAD, reach end
	return STARE_AT, 0
end

function M.on_wake(c)
	-- Nothing to restore: state, mark and goal were saved with it.
end

function M.on_see(c, ev)
	lost_at[c:id()] = nil
	c:set_mark(ev.x, ev.y, ev.z)
	local close, reach = closes_to(c)
	local s = c:state()
	if (s == STARE or s == ATTACK) and ev.dist < close + HOLD_GIVE then
		-- It has them. Facing them is always right; what it does with its
		-- feet is not.
		c:face(ev.x, ev.z)
		if s == STARE then return end
		if c:attacking() then
			-- Committed to this swing: it does not shuffle during a windup,
			-- which is what makes stepping out of it work.
			c:stop()
		elseif ev.dist <= reach then
			c:stop()
			c:attack()
		else
			-- Drifted to the edge of its reach: step back in rather than
			-- stand there while they stand just out of it.
			c:move_to(ev.x, ev.y, ev.z, "walk")
		end
		return
	end
	if ev.dist <= close then
		c:stop()
		c:set_state(reach > 0 and ATTACK or STARE)
		c:face(ev.x, ev.z)
		if reach > 0 then c:attack() end
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
		if c:state() == STARE or c:state() == ATTACK then
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

-- Struck. Wherever it was looking, it now knows where that came from: it
-- turns on them and comes, unless it is already on them.
function M.on_hurt(c, ev)
	lost_at[c:id()] = nil
	heard[c:id()] = nil
	c:set_mark(ev.x, ev.y, ev.z)
	c:face(ev.x, ev.z)
	local s = c:state()
	if s == ATTACK or s == STARE then return end
	local close = closes_to(c)
	if ev.dist <= close then
		c:stop()
		c:set_state(c:attack_reach() > 0 and ATTACK or STARE)
	else
		c:set_state(CHASE)
		c:move_to(ev.x, ev.y, ev.z, "run")
	end
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
