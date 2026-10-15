// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Jaime "Lactozilla" Passos.
// Copyright (C) 2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// Baseline from KartKrewDev/RingRacers src/r_patchrotation.c, v2.4
// commit 7f895c9a7f5ed77a1ccb3e94b326660e744b523e.
// Only function names are changed; the generator adds C++ allocation/enum casts.
// Unmodified v2.4 raster routines; renamed for the Software/base compatibility oracle.
static void Baseline_CalculateDimensions(
	INT32 width, INT32 height,
	fixed_t ca, fixed_t sa,
	INT32 *newwidth, INT32 *newheight)
{
	fixed_t fixedwidth = (width * FRACUNIT);
	fixed_t fixedheight = (height * FRACUNIT);

	fixed_t w1 = abs(FixedMul(fixedwidth, ca) - FixedMul(fixedheight, sa));
	fixed_t w2 = abs(FixedMul(-fixedwidth, ca) - FixedMul(fixedheight, sa));
	fixed_t h1 = abs(FixedMul(fixedwidth, sa) + FixedMul(fixedheight, ca));
	fixed_t h2 = abs(FixedMul(-fixedwidth, sa) + FixedMul(fixedheight, ca));

	w1 = FixedInt(FixedCeil(w1 + (FRACUNIT/2)));
	w2 = FixedInt(FixedCeil(w2 + (FRACUNIT/2)));
	h1 = FixedInt(FixedCeil(h1 + (FRACUNIT/2)));
	h2 = FixedInt(FixedCeil(h2 + (FRACUNIT/2)));

	*newwidth = max(width, max(w1, w2));
	*newheight = max(height, max(h1, h2));
}

void Baseline_DoRotation(rotsprite_t *rotsprite, patch_t *patch, INT32 angle, INT32 xpivot, INT32 ypivot, boolean flip)
{
	patch_t *rotated;
	UINT16 *rawdst, *rawconv;
	size_t size;
	pictureflags_t bflip = (flip) ? PICFLAGS_XFLIP : 0;

	INT32 width = patch->width;
	INT32 height = patch->height;
	INT32 leftoffset = patch->leftoffset;
	INT32 newwidth, newheight;

	fixed_t ca = rollcosang[angle];
	fixed_t sa = rollsinang[angle];
	fixed_t xcenter, ycenter;
	INT32 idx = angle;
	INT32 x, y;
	INT32 sx, sy;
	INT32 dx, dy;
	INT32 ox, oy;
	INT32 minx, miny, maxx, maxy;

	// Don't cache angle = 0
	if (angle < 1 || angle >= ROTANGLES)
		return;

	if (flip)
	{
		idx += rotsprite->angles;
		xpivot = width - xpivot;
		leftoffset = width - leftoffset;
	}

	if (rotsprite->patches[idx])
		return;

	// Find the dimensions of the rotated patch.
	Baseline_CalculateDimensions(width, height, ca, sa, &newwidth, &newheight);

	xcenter = (xpivot * FRACUNIT);
	ycenter = (ypivot * FRACUNIT);

	if (xpivot != width / 2 || ypivot != height / 2)
	{
		newwidth *= 2;
		newheight *= 2;
	}

	minx = newwidth;
	miny = newheight;
	maxx = 0;
	maxy = 0;

	// Draw the rotated sprite to a temporary buffer.
	size = (newwidth * newheight);
	if (!size)
		size = (width * height);
	rawdst = Z_Calloc(size * sizeof(UINT16), PU_STATIC, NULL);

	for (dy = 0; dy < newheight; dy++)
	{
		for (dx = 0; dx < newwidth; dx++)
		{
			x = (dx - (newwidth / 2)) * FRACUNIT;
			y = (dy - (newheight / 2)) * FRACUNIT;
			sx = FixedMul(x, ca) + FixedMul(y, sa) + xcenter;
			sy = -FixedMul(x, sa) + FixedMul(y, ca) + ycenter;

			sx >>= FRACBITS;
			sy >>= FRACBITS;

			if (sx >= 0 && sy >= 0 && sx < width && sy < height)
			{
				void *input = Picture_GetPatchPixel(patch, PICFMT_PATCH, sx, sy, bflip);
				if (input != NULL)
				{
					rawdst[(dy * newwidth) + dx] = (0xFF00 | (*(UINT8 *)input));
					if (dx < minx)
						minx = dx;
					if (dy < miny)
						miny = dy;
					if (dx > maxx)
						maxx = dx;
					if (dy > maxy)
						maxy = dy;
				}
			}
		}
	}

	ox = (newwidth / 2) + (leftoffset - xpivot);
	oy = (newheight / 2) + (patch->topoffset - ypivot);
	width = (maxx - minx);
	height = (maxy - miny);

	if ((unsigned)(width * height) > size)
	{
		UINT16 *src, *dest;

		size = (width * height);
		rawconv = Z_Calloc(size * sizeof(UINT16), PU_STATIC, NULL);

		src = &rawdst[(miny * newwidth) + minx];
		dest = rawconv;
		dy = height;

		while (dy--)
		{
			M_Memcpy(dest, src, width * sizeof(UINT16));
			dest += width;
			src += newwidth;
		}

		ox -= minx;
		oy -= miny;

		Z_Free(rawdst);
	}
	else
	{
		rawconv = rawdst;
		width = newwidth;
		height = newheight;
	}

	// make patch
	rotated = (patch_t *)Picture_Convert(PICFMT_FLAT16, rawconv, PICFMT_PATCH, 0, NULL, width, height, 0, 0, 0);

	Z_ChangeTag(rotated, PU_PATCH_ROTATED);
	Z_SetUser(rotated, (void **)(&rotsprite->patches[idx]));
	Z_Free(rawconv);

	rotated->leftoffset = ox;
	rotated->topoffset = oy;
}

// END
