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
/// \file  r_patchrotation.c
/// \brief Patch rotation.

#include "r_patchrotation.h"
#include "r_things.h" // FEETADJUST
#include "z_zone.h"
#include "w_wad.h"
#include "r_main.h" // R_PointToAngle
#include "k_kart.h" // K_Sliptiding
#include "p_tick.h"

#ifdef ROTSPRITE
fixed_t rollcosang[ROTANGLES];
fixed_t rollsinang[ROTANGLES];

angle_t R_GetPitchRollAngle(mobj_t *mobj, player_t *viewPlayer)
{
	angle_t viewingAngle = R_PointToAnglePlayer(viewPlayer, mobj->x, mobj->y);

	fixed_t pitchMul = -FINESINE(viewingAngle >> ANGLETOFINESHIFT);
	fixed_t rollMul = FINECOSINE(viewingAngle >> ANGLETOFINESHIFT);

	angle_t rollOrPitch = FixedMul(mobj->pitch, pitchMul) + FixedMul(mobj->roll, rollMul);

	return rollOrPitch;
}

static angle_t R_PlayerSpriteRotation(player_t *player, player_t *viewPlayer)
{
	angle_t viewingAngle = R_PointToAnglePlayer(viewPlayer, player->mo->x, player->mo->y);
	angle_t angleDelta = (viewingAngle - player->mo->angle);

	angle_t sliptideLift = player->aizdrifttilt;

	angle_t rollAngle = 0;

	mobj_t *top = K_GetGardenTop(player);

	if (player->mo->eflags & MFE_UNDERWATER)
	{
		rollAngle -= player->underwatertilt;
	}
	else if (sliptideLift)
	{
		/* (from side) tilt downward if turning
		   toward camera, upward if away. */
		rollAngle +=
			FixedMul(sliptideLift, FINESINE(AbsAngle(angleDelta) >> ANGLETOFINESHIFT)) +
			FixedMul(sliptideLift, FINECOSINE(angleDelta >> ANGLETOFINESHIFT));
	}

	if (player->stairjank)
	{
		rollAngle += K_StairJankFlip(ANGLE_11hh / 2 /
				(17 / player->stairjank));
	}

	if (top)
	{
		/* FIXME: why does it not look right at more acute
		   angles without this? There's a related hack to
		   spritexoffset in K_KartPlayerThink. */
		rollAngle += 3 * (INT32)top->rollangle / 2;
	}

	return rollAngle;
}

angle_t R_ModelRotationAngle(mobj_t *mobj, player_t *viewPlayer)
{
	angle_t rollAngle = mobj->rollangle;

	if (mobj->player)
	{
		rollAngle += R_PlayerSpriteRotation(mobj->player, viewPlayer);
	}

	return rollAngle;
}

angle_t R_SpriteRotationAngle(mobj_t *mobj, player_t *viewPlayer)
{
	angle_t rollOrPitch = R_GetPitchRollAngle(mobj, viewPlayer);
	return (rollOrPitch + R_ModelRotationAngle(mobj, viewPlayer));
}

INT32 R_GetRollAngle(angle_t rollangle)
{
	INT32 ra = AngleFixed(rollangle)>>FRACBITS;
#if (ROTANGDIFF > 1)
	ra += (ROTANGDIFF/2);
#endif
	ra /= ROTANGDIFF;
	ra %= ROTANGLES;
	return ra;
}

#define VISROTMUL (ANG1 * ROTANGDIFF)

vector2_t* R_RotateSpriteOffsetsByPitchRoll(
	mobj_t* mobj,
	boolean vflip,
	boolean hflip,
	vector2_t* out)
{
	fixed_t rotcos, rotsin, finx, finy;
	vector2_t xvec, yvec;

	// input offsets
	fixed_t xoffs, yoffs, xpiv, ypiv;

	// final offsets
	INT16 visx, visy, visz;
	INT16 vxpiv, vypiv;

	// visual rotation
	angle_t visrollang;

	// camera angle
	angle_t viewingAngle = R_PointToAngle(mobj->x, mobj->y);

	// rotate ourselves entirely by the sprite's own rotation angle
	angle_t visrot = R_SpriteRotationAngle(mobj, NULL);

	// xoffs = (-cos(xoff) + sin(yoff))
	xoffs =
		FixedMul(mobj->bakeyoff, -FINECOSINE(mobj->angle >> ANGLETOFINESHIFT)) +
		FixedMul(mobj->bakexoff, FINESINE(mobj->angle >> ANGLETOFINESHIFT));
	xpiv =
		FixedMul(mobj->bakeypiv, -FINECOSINE(mobj->angle >> ANGLETOFINESHIFT)) +
		FixedMul(mobj->bakexpiv, FINESINE(mobj->angle >> ANGLETOFINESHIFT));

	// yoffs = (-sin(yoff) + cos(xoff))
	yoffs =
		FixedMul(mobj->bakeyoff, -FINESINE(mobj->angle >> ANGLETOFINESHIFT)) +
		FixedMul(mobj->bakexoff, FINECOSINE(mobj->angle >> ANGLETOFINESHIFT));
	ypiv =
		FixedMul(mobj->bakeypiv, -FINESINE(mobj->angle >> ANGLETOFINESHIFT)) +
		FixedMul(mobj->bakexpiv, FINECOSINE(mobj->angle >> ANGLETOFINESHIFT));

	visrollang = (R_GetRollAngle(visrot) * VISROTMUL) * (hflip ? -1 : 1);

	// get pitch and roll multipliers, mainly used to align the
	// viewpoint with the camera
	fixed_t pitchMul = -FINESINE(viewingAngle >> ANGLETOFINESHIFT);
	fixed_t rollMul = FINECOSINE(viewingAngle >> ANGLETOFINESHIFT);

	// get visual positions
	visz = visy = visx = 0;
	visz = (INT16)(-(mobj->bakezoff / FRACUNIT));
	visx = (INT16)(FixedMul((yoffs / FRACUNIT), rollMul));
	visy = (INT16)(FixedMul((xoffs / FRACUNIT), pitchMul));

	vxpiv = (INT16)(FixedMul((ypiv / FRACUNIT), rollMul));
	vypiv = (INT16)(FixedMul((xpiv / FRACUNIT), pitchMul));

	// rotate by rollangle
	finx = (visx + visy);
	finy = -visz;

	rotcos = FINECOSINE(visrollang >> ANGLETOFINESHIFT);
	rotsin = FINESINE(visrollang >> ANGLETOFINESHIFT);

	xvec.x = FixedMul(finx, rotcos);
	xvec.y = FixedMul(finx, -rotsin);

	yvec.x = FixedMul(finy, rotsin);
	yvec.y = FixedMul(finy, -rotcos);

	// set finalized offsets
	out->x = (fixed_t)(xvec.x + yvec.x + vxpiv + vypiv);
	out->y = (fixed_t)(xvec.y - yvec.y) + (mobj->bakezpiv / FRACUNIT);

	// flip based on vflip and hflip
	// flip the view angle if we're horizontally flipped
	if (hflip)
	{
		out->x *= -1;
	}

	if (vflip)
	{
		out->y *= -1;
	}
	return out;
}

#undef VISROTMUL

patch_t *Patch_GetRotated(patch_t *patch, INT32 angle, boolean flip)
{
	rotsprite_t *rotsprite = patch->rotated;
	if (rotsprite == NULL || angle < 1 || angle >= ROTANGLES)
		return NULL;

	if (flip)
		angle += rotsprite->angles;

	return rotsprite->patches[angle];
}

patch_t *Patch_GetRotatedSprite(
	spriteframe_t *sprite,
	size_t frame, size_t spriteangle,
	boolean flip, boolean adjustfeet,
	void *info, INT32 rotationangle)
{
	rotsprite_t *rotsprite;
	spriteinfo_t *sprinfo = (spriteinfo_t *)info;
	INT32 idx = rotationangle;
	UINT8 type = (adjustfeet ? 1 : 0);

	if (rotationangle < 1 || rotationangle >= ROTANGLES)
		return NULL;

	rotsprite = sprite->rotated[type][spriteangle];

	if (rotsprite == NULL)
	{
		rotsprite = RotatedPatch_Create(ROTANGLES);
		sprite->rotated[type][spriteangle] = rotsprite;
	}

	if (flip)
		idx += rotsprite->angles;

	if (rotsprite->patches[idx] == NULL)
	{
		patch_t *patch;
		INT32 xpivot = 0, ypivot = 0;
		lumpnum_t lump = sprite->lumppat[spriteangle];

		if (lump == LUMPERROR)
			return NULL;

		patch = W_CachePatchNum(lump, PU_SPRITE);

		if (in_bit_array(sprinfo->available, frame))
		{
			xpivot = sprinfo->pivot[frame].x;
			ypivot = sprinfo->pivot[frame].y;
		}
		else if (in_bit_array(sprinfo->available, SPRINFO_DEFAULT_PIVOT))
		{
			xpivot = sprinfo->pivot[SPRINFO_DEFAULT_PIVOT].x;
			ypivot = sprinfo->pivot[SPRINFO_DEFAULT_PIVOT].y;
		}
		else
		{
			xpivot = patch->leftoffset;
			ypivot = patch->height / 2;
		}

		RotatedPatch_DoRotation(rotsprite, patch, rotationangle, xpivot, ypivot, flip);

		//BP: we cannot use special tric in hardware mode because feet in ground caused by z-buffer
		if (adjustfeet)
			((patch_t *)rotsprite->patches[idx])->topoffset += FEETADJUST>>FRACBITS;
	}

	return rotsprite->patches[idx];
}

void Patch_Rotate(patch_t *patch, INT32 angle, INT32 xpivot, INT32 ypivot, boolean flip)
{
	if (patch->rotated == NULL)
		patch->rotated = RotatedPatch_Create(ROTANGLES);
	RotatedPatch_DoRotation(patch->rotated, patch, angle, xpivot, ypivot, flip);
}

rotsprite_t *RotatedPatch_Create(INT32 numangles)
{
	rotsprite_t *rotsprite = Z_Calloc(sizeof(rotsprite_t), PU_STATIC, NULL);
	rotsprite->angles = numangles;
	rotsprite->patches = Z_Calloc(rotsprite->angles * 2 * sizeof(void *), PU_STATIC, NULL);
	return rotsprite;
}

static void RotatedPatch_CalculateDimensions(
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

// Both rasters use this exact inverse mapping; the brightmap's own header
// offsets and opaque bounds must never influence its sampling or output crop.
static void *RotatedPatch_Sample(patch_t *patch, INT32 dx, INT32 dy,
	INT32 canvaswidth, INT32 canvasheight, fixed_t ca, fixed_t sa,
	fixed_t xcenter, fixed_t ycenter, pictureflags_t flip)
{
	fixed_t x = (dx - canvaswidth / 2) * FRACUNIT;
	fixed_t y = (dy - canvasheight / 2) * FRACUNIT;
	INT32 sx = (FixedMul(x, ca) + FixedMul(y, sa) + xcenter) >> FRACBITS;
	INT32 sy = (-FixedMul(x, sa) + FixedMul(y, ca) + ycenter) >> FRACBITS;
	if (sx < 0 || sy < 0 || sx >= patch->width || sy >= patch->height)
		return NULL;
	return Picture_GetPatchPixel(patch, PICFMT_PATCH, sx, sy, flip);
}

#ifdef HWRENDER
static patch_t *rotatedbrightmaps;
static patch_t *invalidatedbrightmaps;
static boolean freeingbrightmaps;

void Patch_FreeInvalidatedBrightmaps(void)
{
	if (freeingbrightmaps)
		return;
	freeingbrightmaps = true;
	while (invalidatedbrightmaps)
	{
		patch_t *entry = invalidatedbrightmaps;
		invalidatedbrightmaps = entry->brightnext;
		entry->brightnext = NULL;
		Patch_Free(entry);
	}
	freeingbrightmaps = false;
}

void Patch_InvalidateRotatedBrightmaps(patch_t *patch)
{
	patch_t **link = &invalidatedbrightmaps;
	// A queued companion may itself be visited by the ongoing tag purge.
	while (*link)
	{
		if (*link == patch)
		{
			*link = patch->brightnext;
			patch->brightnext = NULL;
			break;
		}
		link = &(*link)->brightnext;
	}
	link = &rotatedbrightmaps;
	while (*link)
	{
		patch_t *entry = *link;
		if (entry == patch || entry->brightowner == patch || entry->brightsource == patch)
		{
			patch_t **ownerlink = &entry->brightowner->brightmaps;
			while (*ownerlink != entry)
				ownerlink = &(*ownerlink)->brightownernext;
			*ownerlink = entry->brightownernext;
			*link = entry->brightnext;
			entry->brightownernext = entry->brightowner = entry->brightsource = NULL;
			entry->brightnext = NULL;
			if (entry != patch)
			{
				// Reclaim after the outer free/tag walk, never under its iterator.
				entry->brightnext = invalidatedbrightmaps;
				invalidatedbrightmaps = entry;
			}
		}
		else
			link = &entry->brightnext;
	}
}

patch_t *Patch_GetRotatedBrightmap(patch_t *rotated, patch_t *brightmap)
{
	patch_t *entry;
	patchrotation_t *transform = &rotated->rotation;
	UINT16 *pixels;
	INT32 x, y;
	if (!brightmap || !transform->valid || brightmap->width != transform->width
		|| brightmap->height != transform->height)
		return NULL;
	for (entry = rotated->brightmaps; entry; entry = entry->brightownernext)
		if (entry->brightsource == brightmap)
			return entry;

	pixels = Z_Calloc(rotated->width * rotated->height * sizeof(UINT16), PU_STATIC, NULL);
	for (y = 0; y < rotated->height; ++y)
		for (x = 0; x < rotated->width; ++x)
		{
			UINT8 *pixel = (UINT8 *)RotatedPatch_Sample(brightmap, x + transform->cropx, y + transform->cropy,
				transform->canvaswidth, transform->canvasheight, transform->ca, transform->sa,
				transform->xcenter, transform->ycenter, transform->flip ? PICFLAGS_XFLIP : 0);
			if (pixel)
				pixels[y * rotated->width + x] = 0xFF00 | *pixel;
		}
	entry = (patch_t *)Picture_Convert(PICFMT_FLAT16, pixels, PICFMT_PATCH, 0, NULL,
		rotated->width, rotated->height, 0, 0, 0);
	Z_Free(pixels);
	Z_ChangeTag(entry, PU_PATCH_ROTATED);
	entry->leftoffset = rotated->leftoffset;
	entry->topoffset = rotated->topoffset;
	entry->brightownernext = rotated->brightmaps;
	rotated->brightmaps = entry;
	entry->brightowner = rotated;
	entry->brightsource = brightmap;
	entry->brightnext = rotatedbrightmaps;
	rotatedbrightmaps = entry;
	return entry;
}
#endif

void RotatedPatch_DoRotation(rotsprite_t *rotsprite, patch_t *patch, INT32 angle, INT32 xpivot, INT32 ypivot, boolean flip)
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
	INT32 dx, dy;
	INT32 ox, oy;
	INT32 minx, miny, maxx, maxy;
	INT32 cropx = 0, cropy = 0;

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
	RotatedPatch_CalculateDimensions(width, height, ca, sa, &newwidth, &newheight);

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
			UINT8 *input = (UINT8 *)RotatedPatch_Sample(patch, dx, dy, newwidth, newheight,
				ca, sa, xcenter, ycenter, bflip);
			if (input != NULL)
			{
				rawdst[(dy * newwidth) + dx] = (0xFF00 | *input);
				if (dx < minx) minx = dx;
				if (dy < miny) miny = dy;
				if (dx > maxx) maxx = dx;
				if (dy > maxy) maxy = dy;
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

		cropx = minx;
		cropy = miny;
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
#ifdef HWRENDER
	rotated->rotation.ca = ca;
	rotated->rotation.sa = sa;
	rotated->rotation.xcenter = xcenter;
	rotated->rotation.ycenter = ycenter;
	rotated->rotation.width = patch->width;
	rotated->rotation.height = patch->height;
	rotated->rotation.canvaswidth = newwidth;
	rotated->rotation.canvasheight = newheight;
	rotated->rotation.cropx = cropx;
	rotated->rotation.cropy = cropy;
	rotated->rotation.flip = flip;
	rotated->rotation.valid = true;
#else
	(void)cropx;
	(void)cropy;
#endif
}
#endif
