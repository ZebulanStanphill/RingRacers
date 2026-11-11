-- Make sure the map's scenery objects exist even when a game object
-- already uses their editor numbers.
--
-- The SOC gives the three scenery types these editor numbers. P_GetMobjtype
-- returns the first object type with a matching number, so if the game's own
-- data uses one, the map's things spawn that object (a tree, on Xbox) and
-- the add-on's never appears. This script replaces such objects after the
-- map has spawned its things.
--
-- build.py checks that these numbers match the ones it puts in the SOC and
-- the map.
local THING_TRANS = 3900
local THING_GLOW = 3901
local THING_PARITY = 3902
local DARK_PAD_TAG = 85

local function replaceCollidingScenery()
	if G_BuildMapName(gamemap) ~= "RR_RENDERTEST" then
		return
	end

	-- Looked up here, not at load: the SOC's freeslots don't exist yet when
	-- the engine loads Lua scripts.
	local wanted = {
		[THING_TRANS] = MT_RTTRANS,
		[THING_GLOW] = MT_RTGLOW,
		[THING_PARITY] = MT_RTPARITY,
	}

	-- editor number -> {name of the object that had it, how many things}
	local replaced = {}
	local order = {}
	local spawned = {}

	for mt in mapthings.iterate do
		local want = wanted[mt.type]
		if want ~= nil then
			local old = mt.mobj
			if not (old and old.valid and old.type == want) then
				local name = "no object"
				if old and old.valid then
					name = "MT_" .. mobjinfo[old.type].string
						.. " (mobj type " .. old.type .. ")"
					P_RemoveMobj(old)
				end

				-- Parity objects settle onto their actual floor/slope below.
				local x = mt.x * FRACUNIT
				local y = mt.y * FRACUNIT
				local z = R_PointInSubsector(x, y).sector.floorheight
					+ mt.z * FRACUNIT

				local mo = P_SpawnMobj(x, y, z, want)
				spawned[mt] = mo
				mo.angle = FixedAngle(mt.angle * FRACUNIT)
				mo.scale = FixedMul(mo.scale, mt.scale)
				mo.spritexscale = mt.spritexscale
				mo.spriteyscale = mt.spriteyscale

				local entry = replaced[mt.type]
				if entry == nil then
					entry = {name = name, count = 0}
					replaced[mt.type] = entry
					order[#order + 1] = mt.type
				end
				entry.count = entry.count + 1
			end
			if mt.type == THING_PARITY then
				-- The visual variant lives in thingarg0, never in a global state:
				-- all these objects deliberately share one sprite and state.
				local mo = spawned[mt] or mt.mobj
				-- P_SpawnMobj does not attach a new object to the old mapthing.
				-- The replacement above records it explicitly.
				local variant = mt.args[0]
				mo.frame = A|FF_FULLBRIGHT
				if variant == 1 then
					mo.renderflags = RF_SHADOWDRAW
				elseif variant == 2 then
					mo.frame = $|FF_OVERLAY|FF_TRANS50
				elseif variant == 3 then
					mo.renderflags = RF_OVERLAY|RF_TRANS50
				elseif variant == 4 then
					mo.frame = $|FF_TRANS50
				elseif variant == 5 then
					mo.frame = $|FF_OVERLAY
				end
				-- P_TryMove, even without displacement, sets floorz and
				-- standingslope for a grounded object (p_map.c).
				P_TryMove(mo, mo.x, mo.y, true)
				mo.z = mo.floorz
				P_TryMove(mo, mo.x, mo.y, true)
			end
		end
	end

	if #order == 0 then
		return
	end

	-- One line, so the tester's log names the objects that took the numbers.
	table.sort(order)
	local parts = {}
	for _, number in ipairs(order) do
		local entry = replaced[number]
		parts[#parts + 1] = number .. " was " .. entry.name
			.. ", " .. entry.count .. " things"
	end
	print("Render Test: the game uses the add-on's editor numbers; spawned "
		.. "the add-on's own objects instead: " .. table.concat(parts, "; "))
end

-- MapLoad runs after the engine has spawned the map's things.
addHook("MapLoad", replaceCollidingScenery)

-- DARKNESS_FADE_TIME is eight tics (doomstat.h); race strength is 1/7.
-- Hold for eight seconds, plus the engine's eight-tic fade at each end.
local function darken(player)
	if not player then return end
	if leveltime >= player.darkness_end then
		player.darkness_start = leveltime
	end
	player.darkness_end = leveltime + 8*TICRATE + 16
end

COM_AddCommand("rendertest_darkness", function(player)
	if G_BuildMapName(gamemap) == "RR_RENDERTEST" then
		darken(player)
	end
end)

addHook("PlayerThink", function(player)
	if G_BuildMapName(gamemap) ~= "RR_RENDERTEST" then return end
	local mo = player.mo
	if mo and mo.valid and mo.subsector.sector.tag == DARK_PAD_TAG then
		darken(player)
	end
end)
