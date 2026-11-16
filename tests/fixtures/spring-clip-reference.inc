// A frozen copy of the spring silhouette subtraction as it was before an entry's work was cached per camera
// (src/hardware/hw_main.c at 9f7d3b84a: HWR_SpringSubtract and the helpers that changed), under its own names.
// spring_clip compares the production code against it, entry by entry, over random scenes. Do not update it to follow
// production: it is the reference for what production must keep returning. It uses production's recorded entries
// (hwr_springwalls) and the helpers that did not change (HWR_SpringHalfPlane, HWR_SpringThin, and so on), and keeps
// its own piece buffers and mark list.
static hwr_springtriangle_t *refPieces[2];
static size_t refPieceCapacity[2];
static size_t refPeakPieces;
#ifdef HAVE_VIBERANT
static size_t refMasks[HWR_SPRINGMAXMASKS];
static size_t refNumMasks;
#endif

static INT32 RefClipNear(const hwr_springwall_t *wall, FOutVector *v)
{
	hwr_springplane_t nearplane = {gl_viewcos, 0, gl_viewsin,
		-gl_viewx * gl_viewcos - gl_viewy * gl_viewsin - HWR_SPRINGNEAR};
	INT32 n = HWR_SpringHalfPlane(wall->v, wall->numverts, v, HWR_SPRINGMAXNEAR, &nearplane, true), i;
	if (n < 0)
		return 0;
	// Remove coincident corners at a sloped wall's zero-height endpoint.
	for (i = 0; i < n && n >= 3; )
	{
		INT32 j = (i + 1) % n;
		if (HWR_SpringSamePoint(&v[i], &v[j]))
		{
			memmove(&v[i], &v[i + 1], sizeof(FOutVector) * (n - i - 1));
			n--;
		}
		else i++;
	}
	return n;
}

static boolean RefPlaneApplies(const hwr_springwall_t *plane, const hwr_springview_t *view)
{
	FOutVector v[HWR_SPRINGMAXNEAR];
	double cameraz = plane->a * gl_viewx + plane->b * gl_viewy + plane->c;
	double objectz = plane->a * (view->originx + gl_viewx) + plane->b * (view->originy + gl_viewy) + plane->c;
	double nearest = DBL_MAX, left = DBL_MAX, right = -DBL_MAX, low = DBL_MAX, high = -DBL_MAX;
	INT32 i, n;

	// The spring has to lie wholly on the far side of the plane from the camera.
	if (cameraz < gl_viewz)
	{
		if (view->top >= objectz)
			return false;
	}
	else if (cameraz > gl_viewz)
	{
		if (view->bottom <= objectz)
			return false;
	}
	else
		return false; // Edge-on: Software never draws it.

	n = RefClipNear(plane, v);
	if (n < 3)
		return false;
	for (i = 0; i < n; i++)
	{
		double x = v[i].x - gl_viewx, y = v[i].z - gl_viewy, depth = fmax(HWR_SpringDepth(x, y), 1e-6);
		double screen = HWR_SpringLateral(x, y) / depth, row = (v[i].y - gl_viewz) / depth;
		nearest = fmin(nearest, depth);
		left = fmin(left, screen);
		right = fmax(right, screen);
		low = fmin(low, row);
		high = fmax(high, row);
	}

	// The plane shares screen columns and rows with the sprite.
	if (right < view->left || left > view->right
		|| (view->bottom - gl_viewz) / view->depth > high || (view->top - gl_viewz) / view->depth < low)
		return false;

	// Part of the plane is nearer than the sprite's origin in the sprite's own screen columns: Software looks for
	// a nearer scale of the plane's seg from the sprite's first column to its last, and nowhere else. A plane that
	// passes beside the sprite on its way towards the camera, as the top of a beam does for a spring on the beam's
	// side, isn't drawn over it.
	{
		const double lx = gl_viewsin - view->left * gl_viewcos, lz = -gl_viewcos - view->left * gl_viewsin;
		const double rx = view->right * gl_viewcos - gl_viewsin, rz = view->right * gl_viewsin + gl_viewcos;
		const hwr_springplane_t leftplane = {lx, 0, lz, -(gl_viewx * lx + gl_viewy * lz)};
		const hwr_springplane_t rightplane = {rx, 0, rz, -(gl_viewx * rx + gl_viewy * rz)};
		FOutVector part[HWR_SPRINGMAXNEAR + 2], columns[HWR_SPRINGMAXNEAR + 2];
		INT32 count = HWR_SpringHalfPlane(v, n, part, HWR_SPRINGMAXNEAR + 2, &leftplane, true);
		if (count >= 0)
			count = HWR_SpringHalfPlane(part, count, columns, HWR_SPRINGMAXNEAR + 2, &rightplane, true);
		if (count >= 0) // Too many corners to cut (rounding along a nearly collinear outline): judge by the whole polygon.
		{
			nearest = DBL_MAX;
			for (i = 0; i < count; i++)
				nearest = fmin(nearest, HWR_SpringDepth(columns[i].x - gl_viewx, columns[i].z - gl_viewy));
		}
	}
	return nearest < view->depth;
}

static boolean RefWallApplies(const hwr_springwall_t *wall, const hwr_springview_t *view)
{
	double ax, ay, bx, by, da, db, nearest, farthest;

	if (wall->plane)
		return RefPlaneApplies(wall, view);

	ax = wall->v[0].x - gl_viewx; ay = wall->v[0].z - gl_viewy;
	bx = wall->v[1].x - gl_viewx; by = wall->v[1].z - gl_viewy;
	da = HWR_SpringDepth(ax, ay); db = HWR_SpringDepth(bx, by);
	nearest = fmin(da, db); farthest = fmax(da, db);

	// Scale of the whole side is below the sprite's: it's entirely behind it.
	if (wall->thick ? nearest >= view->depth : nearest > view->depth)
		return false;

	// The silhouette has to share a screen column with the sprite.
	if (nearest > HWR_SPRINGNEAR)
	{
		double sa = HWR_SpringLateral(ax, ay) / da, sb = HWR_SpringLateral(bx, by) / db;
		if (fmax(sa, sb) < view->left || fmin(sa, sb) > view->right)
			return false;
	}

	if (wall->thick)
	{
		// R_CreateDrawNodes compares scale at the sprite's middle screen column,
		// extending the side's line past its ends, not each pixel's depth.
		double cross = view->rayx * (by - ay) - view->rayy * (bx - ax), raydepth;
		if (fabs(cross) < 1e-12)
			return false;
		raydepth = (ax * by - ay * bx) / cross;
		return raydepth > 0 && raydepth < view->depth;
	}

	// R_ClipVisSprite clips by a seg that is wholly nearer than the origin, or that
	// crosses it with the origin on its back side (opposite the camera).
	return farthest <= view->depth || (ax * by - ay * bx)
		* ((bx - ax) * (view->originy - ay) - (by - ay) * (view->originx - ax)) < 0;
}

static INT32 RefWallPlanes(const hwr_springwall_t *wall, hwr_springplane_t *planes)
{
	FOutVector v[HWR_SPRINGMAXNEAR], center;
	INT32 n = RefClipNear(wall, v), i;
	center.x = center.y = center.z = 0;
	if (n < 3 || HWR_SpringFlatSilhouette(v, n))
		return 0;
	for (i = 0; i < n; i++)
	{
		center.x += v[i].x / n;
		center.y += v[i].y / n;
		center.z += v[i].z / n;
	}
	for (i = 0; i < n; i++)
	{
		const FOutVector *a = &v[i], *b = &v[(i + 1) % n];
		double ax = a->x - gl_viewx, ay = a->y - gl_viewz, az = a->z - gl_viewy;
		double bx = b->x - gl_viewx, by = b->y - gl_viewz, bz = b->z - gl_viewy;
		double length;
		hwr_springplane_t *p = &planes[i];
		p->x = ay * bz - az * by;
		p->y = az * bx - ax * bz;
		p->z = ax * by - ay * bx;
		length = sqrt(p->x * p->x + p->y * p->y + p->z * p->z);
		if (length < 1e-12)
			return 0; // Edge through the camera: no finite silhouette.
		p->x /= length;
		p->y /= length;
		p->z /= length;
		p->d = -p->x * gl_viewx - p->y * gl_viewz - p->z * gl_viewy;
		if (HWR_SpringDistance(p, &center) < 0)
		{
			p->x = -p->x; p->y = -p->y; p->z = -p->z; p->d = -p->d;
		}
	}
	return n;
}

static void RefAppend(INT32 buffer, size_t *count, const FOutVector *v, INT32 n)
{
	INT32 i;
	if (!refPieces[buffer])
		refPieceCapacity[buffer] = 0;
	if (*count + n > refPieceCapacity[buffer])
	{
		refPieceCapacity[buffer] = (*count + n) * 2 + 32;
		refPieces[buffer] = (hwr_springtriangle_t *)std::realloc(refPieces[buffer], refPieceCapacity[buffer]
			* sizeof(hwr_springtriangle_t));
	}
	for (i = 1; i + 1 < n; i++)
	{
		hwr_springtriangle_t *t;
		if (HWR_SpringSamePoint(&v[0], &v[i]) || HWR_SpringSamePoint(&v[i], &v[i + 1])
			|| HWR_SpringSamePoint(&v[0], &v[i + 1]) || HWR_SpringThin(&v[0], &v[i], &v[i + 1]))
			continue; // Repeated corner or a sliver: no pixel to cover.
		t = &refPieces[buffer][(*count)++];
		t->v[0] = v[0]; t->v[1] = v[i]; t->v[2] = v[i + 1];
	}
}

static boolean RefProbeOverlaps(const hwr_springwall_t *entry, const hwr_springview_t *view)
{
	FOutVector v[HWR_SPRINGMAXNEAR];
	double left = DBL_MAX, right = -DBL_MAX, low = DBL_MAX, high = -DBL_MAX;
	INT32 i, n = RefClipNear(entry, v);
	if (n < 2)
		return false;
	for (i = 0; i < n; i++)
	{
		double x = v[i].x - gl_viewx, y = v[i].z - gl_viewy, depth = fmax(HWR_SpringDepth(x, y), 1e-6);
		double screen = HWR_SpringLateral(x, y) / depth, row = (v[i].y - gl_viewz) / depth;
		left = fmin(left, screen); right = fmax(right, screen);
		low = fmin(low, row); high = fmax(high, row);
	}
	return right >= view->left && left <= view->right
		&& (view->bottom - gl_viewz) / view->depth <= high && (view->top - gl_viewz) / view->depth >= low;
}

static boolean RefSubtract(const FOutVector *quad, const FOutVector *area, FBITFIELD flags, gl_vissprite_t *spr,
	boolean mask, INT32 *piecebuffer, size_t *piececount)
{
	hwr_springview_t view;
	size_t w, i, count = 0, farthestthick;
	INT32 buffer = 0;
	boolean changed = false;
	const boolean probe = hwr_springprobe && !mask; // the marking pass repeats one already logged
	if (!hwr_numspringwalls || !(spr->mobj->flags & MF_SPRING) || (flags & PF_NoDepthTest)
		|| !HWR_SpringSetView(&view, quad, spr->gx, spr->gy, spr->gz, spr->gzt))
	{
		if (probe && (spr->mobj->flags & MF_SPRING))
			I_OutputMsg("Spring probe: type=%d at (%.1f,%.1f,%.1f) left alone: entries=%u flags=0x%lx\n", (int)spr->mobj->type,
				FIXED_TO_FLOAT(spr->mobj->x), FIXED_TO_FLOAT(spr->mobj->y), FIXED_TO_FLOAT(spr->mobj->z),
				(unsigned)hwr_numspringwalls, (unsigned long)flags);
		return false;
	}
	RefAppend(buffer, &count, area, 4);
	// R_CreateDrawNodes inserts the sprite before the farthest 3D floor side or plane it sorts behind, so every
	// one drawn after that (nearer in the BSP order, recorded earlier here) paints over it as well.
	farthestthick = SIZE_MAX;
	for (w = 0; w < hwr_numspringwalls; w++)
		if (hwr_springwalls[w].thick && RefWallApplies(&hwr_springwalls[w], &view))
			farthestthick = w;
	if (probe)
		I_OutputMsg("Spring probe: type=%d at (%.1f,%.1f,%.1f) gz=%.1f gzt=%.1f quad=(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f) depth=%.2f entries=%u farthestthick=%ld\n",
			(int)spr->mobj->type, FIXED_TO_FLOAT(spr->mobj->x), FIXED_TO_FLOAT(spr->mobj->y), FIXED_TO_FLOAT(spr->mobj->z), spr->gz, spr->gzt,
			quad[0].x, quad[0].z, quad[0].y, quad[1].x, quad[1].z, quad[1].y, quad[2].x, quad[2].z, quad[2].y, quad[3].x, quad[3].z, quad[3].y,
			view.depth, (unsigned)hwr_numspringwalls, farthestthick == SIZE_MAX ? -1L : (long)farthestthick);
	for (w = 0; w < hwr_numspringwalls && count && count < HWR_SPRINGMAXPIECES; w++)
	{
		hwr_springplane_t planes[HWR_SPRINGMAXNEAR];
		INT32 edges, e;
		size_t nextcount = 0;
		boolean subtracted = hwr_springwalls[w].thick ? (farthestthick != SIZE_MAX && w <= farthestthick)
			: RefWallApplies(&hwr_springwalls[w], &view), cut = false;
		if (!subtracted || !(edges = RefWallPlanes(&hwr_springwalls[w], planes)))
		{
			if (probe && RefProbeOverlaps(&hwr_springwalls[w], &view))
				HWR_SpringProbeEntry(w, &hwr_springwalls[w], RefWallApplies(&hwr_springwalls[w], &view), subtracted, false);
			continue;
		}
		for (i = 0; i < count; i++)
		{
			const FOutVector *tri = refPieces[buffer][i].v;
			FOutVector inside[HWR_SPRINGMAXCLIP], clipped[HWR_SPRINGMAXCLIP], outside[HWR_SPRINGMAXCLIP];
			size_t mark = nextcount;
			INT32 n = 3;
			boolean overflow = false;
			// A separating edge rejects this entire silhouette without splitting.
			for (e = 0; e < edges; e++)
				if (HWR_SpringDistance(&planes[e], &tri[0]) <= 0
					&& HWR_SpringDistance(&planes[e], &tri[1]) <= 0
					&& HWR_SpringDistance(&planes[e], &tri[2]) <= 0)
					break;
			if (e == edges)
			{
				cut = true;
				memcpy(inside, tri, sizeof(FOutVector) * 3);
				for (e = 0; e < edges && n >= 3; e++)
				{
					INT32 out = HWR_SpringHalfPlane(inside, n, outside, HWR_SPRINGMAXCLIP, &planes[e], false);
					if (out < 0)
					{
						overflow = true;
						break;
					}
					RefAppend(1 - buffer, &nextcount, outside, out);
					n = HWR_SpringHalfPlane(inside, n, clipped, HWR_SPRINGMAXCLIP, &planes[e], true);
					if (n < 0)
					{
						overflow = true;
						break;
					}
					memcpy(inside, clipped, sizeof(FOutVector) * n);
				}
			}
			if (!overflow && e == edges && n >= 3)
				changed = true; // The parts outside the silhouette were kept above.
			else
			{
				nextcount = mark; // Missed the silhouette, or its cuts outgrew the buffers: keep the triangle whole.
				RefAppend(1 - buffer, &nextcount, tri, 3);
			}
		}
		buffer = 1 - buffer;
		count = nextcount;
		if (count > refPeakPieces)
			refPeakPieces = count;
#ifdef HAVE_VIBERANT
		if (mask && cut && refNumMasks < HWR_SPRINGMAXMASKS
			&& VibeRant_DrawSpringMask(hwr_springwalls[w].v, hwr_springwalls[w].numverts, true))
			refMasks[refNumMasks++] = w;
#endif
		if (probe && (cut || RefProbeOverlaps(&hwr_springwalls[w], &view)))
			HWR_SpringProbeEntry(w, &hwr_springwalls[w], hwr_springwalls[w].thick
				? RefWallApplies(&hwr_springwalls[w], &view) : true, true, cut);
	}
	if (probe)
	{
		double left = 0;
		if (!changed)
			left = HWR_SpringScreenArea(quad, 4);
		else
			for (i = 0; i < count; i++)
				left += HWR_SpringScreenArea(refPieces[buffer][i].v, 3);
		I_OutputMsg("Spring result: pieces=%u changed=%d stopped=%d area=%.6f of %.6f\n", (unsigned)(changed ? count : 1),
			(int)changed, (int)(count >= HWR_SPRINGMAXPIECES), left, HWR_SpringScreenArea(quad, 4));
	}
	*piecebuffer = buffer;
	*piececount = count;
	return changed;
}
