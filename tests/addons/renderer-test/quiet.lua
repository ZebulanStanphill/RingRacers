-- Keep renderer checks free of AI racers, including after a map restart.
local function removeTestMapBots()
	if G_BuildMapName(gamemap) ~= "RR_RENDERTEST" then
		return
	end

	-- K_RemoveBot invalidates its player userdata. Finish players.iterate
	-- before removing anyone: the iterator needs its previous player intact.
	local bots = {}
	for player in players.iterate do
		if player.bot then
			bots[#bots + 1] = player
		end
	end
	for _, bot in ipairs(bots) do
		K_RemoveBot(bot)
	end
end

-- MapLoad runs after the engine fills the race and spawns its players.
addHook("MapLoad", removeTestMapBots)
-- Also catch bots added later, without changing the user's bot settings.
addHook("ThinkFrame", removeTestMapBots)
