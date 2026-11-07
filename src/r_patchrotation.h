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
/// \file  r_patchrotation.h
/// \brief Patch rotation.

#include "r_patch.h"
#include "r_picformats.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ROTSPRITE
rotsprite_t *RotatedPatch_Create(INT32 numangles);
void RotatedPatch_DoRotation(rotsprite_t *rotsprite, patch_t *patch, INT32 angle, INT32 xpivot, INT32 ypivot, boolean flip);

extern fixed_t rollcosang[ROTANGLES];
extern fixed_t rollsinang[ROTANGLES];

#ifdef __cplusplus
} // extern "C"
#endif

#endif

#if defined(ROTSPRITE) && defined(HAVE_D3D12) && defined(HWRENDER)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct
{
	patchrotation_t transform;
	INT32 width, height, leftoffset, topoffset;
} rollgeometry_t;
void RotatedPatch_Geometry(rollgeometry_t *out, patch_t *patch,
	INT32 angle, INT32 xpivot, INT32 ypivot, boolean flip);
const rollgeometry_t *Patch_GetRollGeometry(spriteframe_t *sprite, size_t frame,
	size_t slot, boolean flip, void *info, INT32 angle);
extern UINT32 hwr_roll_shader, hwr_roll_cpu, hwr_roll_geometry;
#ifdef __cplusplus
}
#endif
#endif
