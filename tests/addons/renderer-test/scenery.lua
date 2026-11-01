-- Make sure the map's two scenery objects exist even when a game object
-- already uses their editor numbers.
--
-- The SOC gives MT_RTTRANS and MT_RTGLOW these editor numbers. P_GetMobjtype
-- returns the first object type with a matching number, so if the game's own
-- data uses one, the map's things spawn that object (a tree, on Xbox) and
-- the add-on's never appears. This script replaces such objects after the
-- map has spawned its things.
--
-- build.py checks that these numbers match the ones it puts in the SOC and
-- the map.
local THING_TRANS = 3900
local THING_GLOW = 3901

local function replaceCollidingScenery()
	if G_BuildMapName(gamemap) ~= "RR_RENDERTEST" then
		return
	end

	-- Looked up here, not at load: the SOC's freeslots don't exist yet when
	-- the engine loads Lua scripts.
	local wanted = {
		[THING_TRANS] = MT_RTTRANS,
		[THING_GLOW] = MT_RTGLOW,
	}

	-- editor number -> {name of the object that had it, how many things}
	local replaced = {}
	local order = {}

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

				-- The map's rooms have flat floors, so the floor under the
				-- thing is the same as at the engine's own spawn point.
				local x = mt.x * FRACUNIT
				local y = mt.y * FRACUNIT
				local z = R_PointInSubsector(x, y).sector.floorheight
					+ mt.z * FRACUNIT

				local mo = P_SpawnMobj(x, y, z, want)
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
