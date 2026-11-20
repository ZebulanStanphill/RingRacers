// Frozen from upstream's P_BlockThingsIterator (src/p_maputl.c), which PIT_CheckThing was called through.
static boolean P_BlockThingsIterator(INT32 x, INT32 y, BlockItReturn_t (*func)(mobj_t *))
{
	mobj_t *mobj, *bnext = NULL;

	if (x < 0 || y < 0 || x >= bmapwidth || y >= bmapheight)
		return true;

	// Check interaction with the objects in the blockmap.
	for (mobj = blocklinks[y*bmapwidth + x]; mobj; mobj = bnext)
	{
		BlockItReturn_t ret = BMIT_CONTINUE;

		P_SetTarget(&bnext, mobj->bnext); // We want to note our reference to bnext here incase it is MF_NOTHINK and gets removed!
		ret = func(mobj);

		if (ret == BMIT_ABORT)
		{
			P_SetTarget(&bnext, NULL);
			return false; // failure
		}

		if ((ret == BMIT_STOP)
			|| (bnext && P_MobjWasRemoved(bnext))) // func just broke blockmap chain, cannot continue.
		{
			P_SetTarget(&bnext, NULL);
			return true; // success
		}
	}

	return true;
}
