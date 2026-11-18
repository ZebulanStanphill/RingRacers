// Frozen pre-table blockmap walk and callback rejection rules.

// Model the old per-line fields independently of the production table.
struct ReferenceLine
{
	fixed_t bbox[4];
	size_t validcount;
};
static ReferenceLine *referenceLines;

static boolean ReferenceBlockLinesIterator(INT32 x, INT32 y, BlockItReturn_t (*func)(line_t *))
{
	INT32 offset;
	const INT32 *list; // Big blockmap
	polymaplink_t *plink; // haleyjd 02/22/06
	line_t *ld;

	if (x < 0 || y < 0 || x >= bmapwidth || y >= bmapheight)
		return true;

	offset = y*bmapwidth + x;

	// haleyjd 02/22/06: consider polyobject lines
	plink = polyblocklinks[offset];

	while (plink)
	{
		polyobj_t *po = plink->po;

		if (po->validcount != validcount) // if polyobj hasn't been checked
		{
			size_t i;
			po->validcount = validcount;

			for (i = 0; i < po->numLines; ++i)
			{
				BlockItReturn_t ret = BMIT_CONTINUE;
				ReferenceLine &ref = referenceLines[po->lines[i] - lines];

				if (ref.validcount == validcount) // line has been checked
					continue;

				ref.validcount = validcount;
				ret = func(po->lines[i]);

				if (ret == BMIT_ABORT)
				{
					return false;
				}
				else if (ret == BMIT_STOP)
				{
					return true;
				}
			}
		}
		plink = (polymaplink_t *)(plink->link.next);
	}

	offset = *(blockmap + offset); // offset = blockmap[y*bmapwidth+x];

	// First index is really empty, so +1 it.
	for (list = blockmaplump + offset + 1; *list != -1; list++)
	{
		BlockItReturn_t ret = BMIT_CONTINUE;

		ld = &lines[*list];
		ReferenceLine &ref = referenceLines[*list];

		if (ref.validcount == validcount)
			continue; // Line has already been checked.

		ref.validcount = validcount;
		ret = func(ld);

		if (ret == BMIT_ABORT)
		{
			return false;
		}
		else if (ret == BMIT_STOP)
		{
			return true;
		}
	}
	return true; // Everything was checked.
}

static BlockItReturn_t ReferenceCheckLine(line_t *ld)
{
	// Was g_tm.thing->z + g_tm.thing->height: the test's MoverTop reports the fault of a cleared g_tm.thing.
	const fixed_t thingtop = MoverTop(ld);
	opening_t open = {0};
	(void)thingtop;
	(void)open;

	if (ld->polyobj && !(ld->polyobj->flags & POF_SOLID))
		return BMIT_CONTINUE;

	const fixed_t *linebox = referenceLines[ld - lines].bbox;
	if (g_tm.bbox[BOXRIGHT] <= linebox[BOXLEFT] || g_tm.bbox[BOXLEFT] >= linebox[BOXRIGHT]
	|| g_tm.bbox[BOXTOP] <= linebox[BOXBOTTOM] || g_tm.bbox[BOXBOTTOM] >= linebox[BOXTOP])
		return BMIT_CONTINUE;

	return Record(ld);
}

static BlockItReturn_t ReferenceGetSectors(line_t *ld)
{
	const fixed_t *linebox = referenceLines[ld - lines].bbox;
	if (g_tm.bbox[BOXRIGHT] <= linebox[BOXLEFT] ||
		g_tm.bbox[BOXLEFT] >= linebox[BOXRIGHT] ||
		g_tm.bbox[BOXTOP] <= linebox[BOXBOTTOM] ||
		g_tm.bbox[BOXBOTTOM] >= linebox[BOXTOP])
	return BMIT_CONTINUE;

	return Record(ld);
}

static BlockItReturn_t ReferenceGetPrecipSectors(line_t *ld)
{
	const fixed_t *linebox = referenceLines[ld - lines].bbox;
	if (g_tm.precipbbox[BOXRIGHT] <= linebox[BOXLEFT] ||
		g_tm.precipbbox[BOXLEFT] >= linebox[BOXRIGHT] ||
		g_tm.precipbbox[BOXTOP] <= linebox[BOXBOTTOM] ||
		g_tm.precipbbox[BOXBOTTOM] >= linebox[BOXTOP])
	return BMIT_CONTINUE;

	return Record(ld);
}

// The reference still invokes every callback. This is which calls a box
// walk must keep, before any callback-specific rejection such as solidity.
static bool ReferenceBoxReject(const fixed_t *bbox, const line_t *ld)
{
	const fixed_t *linebox = referenceLines[ld - lines].bbox;
	return bbox[BOXRIGHT] <= linebox[BOXLEFT] || bbox[BOXLEFT] >= linebox[BOXRIGHT]
		|| bbox[BOXTOP] <= linebox[BOXBOTTOM] || bbox[BOXBOTTOM] >= linebox[BOXTOP];
}
