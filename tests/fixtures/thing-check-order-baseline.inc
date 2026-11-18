	// Frozen from upstream PIT_CheckThing, from the self test to paper collision.
	// don't clip against self
	if (thing == g_tm.thing)
		return BMIT_CONTINUE;

	// Ignore spectators
	if ((g_tm.thing->player && g_tm.thing->player->spectator)
	|| (thing->player && thing->player->spectator))
		return BMIT_CONTINUE;

	// Ignore the collision if BOTH things are in hitlag.
	if (P_MobjIsFrozen(thing) && P_MobjIsFrozen(g_tm.thing))
		return BMIT_CONTINUE;

	if ((thing->flags & MF_NOCLIPTHING) || !(thing->flags & (MF_SOLID|MF_SPECIAL|MF_PAIN|MF_SHOOTABLE|MF_SPRING)))
		return BMIT_CONTINUE;

	// Thing is respawning
	if (P_MobjIsReappearing(thing))
		return BMIT_CONTINUE;

	blockdist = thing->radius + g_tm.thing->radius;

	if (abs(thing->x - g_tm.x) >= blockdist || abs(thing->y - g_tm.y) >= blockdist)
		return BMIT_CONTINUE; // didn't hit it
