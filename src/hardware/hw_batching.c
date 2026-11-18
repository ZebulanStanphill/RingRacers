// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_batching.c
/// \brief Draw call batching and related things.

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_batching.h"
// [ZE]: Include the hardware driver interface, for the run-time driver checks below.
#include "hw_driver.h"
#include "../i_system.h"

// The texture for the next polygon given to HWR_ProcessPolygon.
// Set with HWR_SetCurrentTexture.
GLMipmap_t *current_texture = NULL;
GLMipmap_t *current_brightmap = NULL;

boolean currently_batching = false;

FOutVector* finalVertexArray = NULL;// contains subset of sorted vertices and texture coordinates to be sent to gpu
UINT32* finalVertexIndexArray = NULL;// contains indexes for glDrawElements, taking into account fan->triangles conversion
//     NOTE have this alloced as 3x finalVertexArray size
int finalVertexArrayAllocSize = 65536;
//GLubyte* colorArray = NULL;// contains color data to be sent to gpu, if needed
//int colorArrayAllocSize = 65536;
// not gonna use this for now, just sort by color and change state when it changes
// later maybe when using vertex attributes if it's needed

PolygonArrayEntry* polygonArray = NULL;// contains the polygon data from DrawPolygon, waiting to be processed
int polygonArraySize = 0;
UINT32* polygonIndexArray = NULL;// contains sorting pointers for polygonArray
int polygonArrayAllocSize = 65536;

FOutVector* unsortedVertexArray = NULL;// contains unsorted vertices and texture coordinates from DrawPolygon
int unsortedVertexArraySize = 0;
int unsortedVertexArrayAllocSize = 65536;

// Enables batching mode. HWR_ProcessPolygon will collect polygons instead of passing them directly to the rendering backend.
// Call HWR_RenderBatches to render all the collected geometry.
void HWR_StartBatching(void)
{
	if (currently_batching)
		I_Error("Repeat call to HWR_StartBatching without HWR_RenderBatches");

	// init arrays if that has not been done yet
	if (!finalVertexArray)
	{
		finalVertexArray = malloc(finalVertexArrayAllocSize * sizeof(FOutVector));
		finalVertexIndexArray = malloc(finalVertexArrayAllocSize * 3 * sizeof(UINT32));
		polygonArray = malloc(polygonArrayAllocSize * sizeof(PolygonArrayEntry));
		polygonIndexArray = malloc(polygonArrayAllocSize * sizeof(UINT32));
		unsortedVertexArray = malloc(unsortedVertexArrayAllocSize * sizeof(FOutVector));
	}

	currently_batching = true;
}

// This replaces the direct calls to pfnSetTexture in cases where batching is available.
// The texture selection is saved for the next HWR_ProcessPolygon call.
// Doing this was easier than getting a texture pointer to HWR_ProcessPolygon.
void HWR_SetCurrentTexture(GLMipmap_t *texture)
{
	if (currently_batching)
	{
		if (texture != NULL)
		{
			if (texture->flags & TF_BRIGHTMAP)
			{
				current_brightmap = texture;
			}
			else
			{
				current_texture = texture;
				current_brightmap = NULL;
			}
		}
		else
		{
			current_texture = current_brightmap = NULL;
		}
	}
	else
	{
		HWD.pfnSetTexture(texture);
	}
}

// If batching is enabled, this function collects the polygon data and the chosen texture
// for later use in HWR_RenderBatches. Otherwise the rendering backend is used to
// render the polygon immediately.
void HWR_ProcessPolygon(FSurfaceInfo *pSurf, FOutVector *pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, int shader, boolean horizonSpecial)
{
	if (currently_batching)
	{
		if (!pSurf)
			I_Error("Got a null FSurfaceInfo in batching");// nulls should not come in the stuff that batching currently applies to
		if (polygonArraySize == polygonArrayAllocSize)
		{
			PolygonArrayEntry* new_array;
			// ran out of space, make new array double the size
			polygonArrayAllocSize *= 2;
			new_array = malloc(polygonArrayAllocSize * sizeof(PolygonArrayEntry));
			memcpy(new_array, polygonArray, polygonArraySize * sizeof(PolygonArrayEntry));
			free(polygonArray);
			polygonArray = new_array;
			// also need to redo the index array, dont need to copy it though
			free(polygonIndexArray);
			polygonIndexArray = malloc(polygonArrayAllocSize * sizeof(UINT32));
		}

		while (unsortedVertexArraySize + (int)iNumPts > unsortedVertexArrayAllocSize)
		{
			FOutVector* new_array;
			// need more space for vertices in unsortedVertexArray
			unsortedVertexArrayAllocSize *= 2;
			new_array = malloc(unsortedVertexArrayAllocSize * sizeof(FOutVector));
			memcpy(new_array, unsortedVertexArray, unsortedVertexArraySize * sizeof(FOutVector));
			free(unsortedVertexArray);
			unsortedVertexArray = new_array;
		}

		// add the polygon data to the arrays

		polygonArray[polygonArraySize].surf = *pSurf;
		polygonArray[polygonArraySize].vertsIndex = unsortedVertexArraySize;
		polygonArray[polygonArraySize].numVerts = iNumPts;
		polygonArray[polygonArraySize].polyFlags = PolyFlags;
		polygonArray[polygonArraySize].brightmap = current_brightmap;
		polygonArray[polygonArraySize].texture = current_texture;
		polygonArray[polygonArraySize].shader = shader;
		polygonArray[polygonArraySize].horizonSpecial = horizonSpecial;
		polygonArraySize++;

		memcpy(&unsortedVertexArray[unsortedVertexArraySize], pOutVerts, iNumPts * sizeof(FOutVector));
		unsortedVertexArraySize += iNumPts;
	}
	else
	{
		if (shader)
			HWD.pfnSetShader(shader);
		HWD.pfnDrawPolygon(pSurf, pOutVerts, iNumPts, PolyFlags);
	}
}

// [ZE]: What the sort compares, copied out of polygonArray so that comparisons stay in one
// contiguous array instead of chasing large entries and texture pointers.
// The fields are in sort order; index is the final tiebreak, which keeps the sort
// total and deterministic.
typedef struct
{
	// [ZE]: A polygon's sort key, in sort order. Shader -2 draws a masked middle texture between two skies before the sky occluders.
	INT32 shader;// -1 for skywalls and horizon lines, which must retain their order; -2 for PF_BeforeSky, drawn before them
	UINT32 texture;// the texture's `downloaded` name, or 0
	UINT32 brightmap;
	UINT64 polyFlags;
	UINT32 polyColor, tintColor, fadeColor;
	UINT32 lightLevel, fadeStart, fadeEnd, directional, lightTableId;
	UINT32 index;
} PolygonSortKey;

// [ZE]: One compact key per polygon, so sorting does not chase pointers.
static PolygonSortKey *polygonSortKeys = NULL;
static int polygonSortKeysAllocSize = 0;

// [ZE]: Compares one field of two keys, falling through to the next field when equal.
#define COMPARE_KEY_FIELD(field) \
	if (key1->field != key2->field) return key1->field < key2->field ? -1 : 1

// [ZE]: Compares two sort keys, which replaces comparing whole polygon entries.
static int comparePolygonKeys(const void *p1, const void *p2)
{
	const PolygonSortKey *key1 = (const PolygonSortKey *)p1;
	const PolygonSortKey *key2 = (const PolygonSortKey *)p2;
	COMPARE_KEY_FIELD(shader);
	COMPARE_KEY_FIELD(texture);
	COMPARE_KEY_FIELD(brightmap);
	COMPARE_KEY_FIELD(polyFlags);
	COMPARE_KEY_FIELD(polyColor);
	COMPARE_KEY_FIELD(tintColor);
	COMPARE_KEY_FIELD(fadeColor);
	COMPARE_KEY_FIELD(lightLevel);
	COMPARE_KEY_FIELD(fadeStart);
	COMPARE_KEY_FIELD(fadeEnd);
	COMPARE_KEY_FIELD(directional);
	COMPARE_KEY_FIELD(lightTableId);
	COMPARE_KEY_FIELD(index);
	return 0;
}

// [ZE]: Each group keeps its first polygon and a count that becomes its write position.
typedef struct
{
	UINT32 representative;
	UINT32 count;
} PolygonSortGroup;

// [ZE]: Reuse the grouping scratch space across frames. UINT32_MAX marks an empty slot.
static UINT32 *polygonGroupSlots = NULL;
static PolygonSortGroup *polygonSortGroups = NULL;
static UINT32 *polygonGroupOrder = NULL;
static UINT32 *polygonGroupForIndex = NULL;
static size_t polygonGroupTableCapacity = 0;

// [ZE]: Hash every field that defines a group, including both halves of the flags.
static UINT32 hashPolygonGroupKey(const PolygonSortKey *key)
{
	UINT32 hash = 2166136261U;
#define MIX_GROUP_FIELD(value) hash = (hash ^ (UINT32)(value)) * 16777619U
	MIX_GROUP_FIELD(key->shader);
	MIX_GROUP_FIELD(key->texture);
	MIX_GROUP_FIELD(key->brightmap);
	MIX_GROUP_FIELD(key->polyFlags);
	MIX_GROUP_FIELD(key->polyFlags >> 32);
	MIX_GROUP_FIELD(key->polyColor);
	MIX_GROUP_FIELD(key->tintColor);
	MIX_GROUP_FIELD(key->fadeColor);
	MIX_GROUP_FIELD(key->lightLevel);
	MIX_GROUP_FIELD(key->fadeStart);
	MIX_GROUP_FIELD(key->fadeEnd);
	MIX_GROUP_FIELD(key->directional);
	MIX_GROUP_FIELD(key->lightTableId);
#undef MIX_GROUP_FIELD

	// [ZE]: Mix high bits into the low bits used to choose a slot.
	hash ^= hash >> 16;
	hash *= 0x85ebca6bU;
	return hash ^ (hash >> 13);
}

// [ZE]: Resolve hash collisions by comparing fields, excluding the index tiebreak.
static boolean equalPolygonGroupKeys(const PolygonSortKey *key1, const PolygonSortKey *key2)
{
	return key1->shader == key2->shader
		&& key1->texture == key2->texture
		&& key1->brightmap == key2->brightmap
		&& key1->polyFlags == key2->polyFlags
		&& key1->polyColor == key2->polyColor
		&& key1->tintColor == key2->tintColor
		&& key1->fadeColor == key2->fadeColor
		&& key1->lightLevel == key2->lightLevel
		&& key1->fadeStart == key2->fadeStart
		&& key1->fadeEnd == key2->fadeEnd
		&& key1->directional == key2->directional
		&& key1->lightTableId == key2->lightTableId;
}

// [ZE]: Sort group numbers by their representative polygons using the original order.
static int comparePolygonGroups(const void *p1, const void *p2)
{
	const PolygonSortGroup *group1 = &polygonSortGroups[*(const UINT32 *)p1];
	const PolygonSortGroup *group2 = &polygonSortGroups[*(const UINT32 *)p2];
	return comparePolygonKeys(&polygonSortKeys[group1->representative], &polygonSortKeys[group2->representative]);
}

// [ZE]: Grow all scratch arrays together. At most half the slots can be occupied.
static void reservePolygonGroups(size_t tableSize)
{
	const size_t capacity = tableSize / 2;
	if (tableSize <= polygonGroupTableCapacity)
		return;
	if (tableSize > SIZE_MAX / sizeof(*polygonGroupSlots) || capacity > SIZE_MAX / sizeof(*polygonSortGroups))
		I_Error("Polygon sort scratch space is too large");

	free(polygonGroupSlots);
	free(polygonSortGroups);
	free(polygonGroupOrder);
	free(polygonGroupForIndex);
	polygonGroupSlots = malloc(tableSize * sizeof(*polygonGroupSlots));
	polygonSortGroups = malloc(capacity * sizeof(*polygonSortGroups));
	polygonGroupOrder = malloc(capacity * sizeof(*polygonGroupOrder));
	polygonGroupForIndex = malloc(capacity * sizeof(*polygonGroupForIndex));
	if (!polygonGroupSlots || !polygonSortGroups || !polygonGroupOrder || !polygonGroupForIndex)
		I_Error("Could not allocate polygon sort scratch space");
	polygonGroupTableCapacity = tableSize;
}

// [ZE]: Group equal keys before sorting. Scatter polygons in submission order to
// keep the original index tiebreak within each group.
static void sortPolygonIndices(UINT32 *indices, UINT32 count)
{
	size_t tableSize = 2;
	size_t mask;
	UINT32 numGroups = 0;
	UINT32 offset = 0;
	UINT32 i;

	if (!count)
		return;
	while (tableSize / 2 < count)
	{
		if (tableSize > SIZE_MAX / 2)
			I_Error("Polygon sort table is too large");
		tableSize *= 2;
	}
	reservePolygonGroups(tableSize);
	mask = tableSize - 1;
	// [ZE]: Clear only the slots used by this frame, even after a larger frame.
	memset(polygonGroupSlots, 0xff, tableSize * sizeof(*polygonGroupSlots));

	// [ZE]: Assign group numbers in order of first appearance using linear probing.
	for (i = 0; i < count; i++)
	{
		const PolygonSortKey *key = &polygonSortKeys[i];
		size_t slot = hashPolygonGroupKey(key) & mask;
		UINT32 group = polygonGroupSlots[slot];
		while (group != UINT32_MAX
			&& !equalPolygonGroupKeys(key, &polygonSortKeys[polygonSortGroups[group].representative]))
		{
			slot = (slot + 1) & mask;
			group = polygonGroupSlots[slot];
		}
		if (group == UINT32_MAX)
		{
			group = numGroups++;
			polygonGroupSlots[slot] = group;
			polygonSortGroups[group].representative = i;
			polygonSortGroups[group].count = 0;
			polygonGroupOrder[group] = group;
		}
		polygonGroupForIndex[i] = group;
		polygonSortGroups[group].count++;
	}

	// [ZE]: Turn each group's count into its starting position in the sorted array.
	qsort(polygonGroupOrder, numGroups, sizeof(*polygonGroupOrder), comparePolygonGroups);
	for (i = 0; i < numGroups; i++)
	{
		PolygonSortGroup *group = &polygonSortGroups[polygonGroupOrder[i]];
		UINT32 start = offset;
		offset += group->count;
		group->count = start;
	}

	// [ZE]: Each group's members occupy consecutive positions in ascending index order.
	for (i = 0; i < count; i++)
		indices[polygonSortGroups[polygonGroupForIndex[i]].count++] = i;
}

// [ZE]: Fills the sort key of every polygon. With shaders, the order is shader, texture,
// brightmap, polyflags, colors and light; without, texture, polyflags and color.
static void buildPolygonSortKeys(boolean shaders)
{
	int i;
	for (i = 0; i < polygonArraySize; i++)
	{
		const PolygonArrayEntry *poly = &polygonArray[i];
		PolygonSortKey *key = &polygonSortKeys[i];
		boolean keepOrder = poly->polyFlags & PF_NoTexture || poly->horizonSpecial;

		// [ZE]: Build the key from zero, then fill in only the fields that the order uses.
		memset(key, 0, sizeof *key);
		key->index = i;
		if (!shaders)
			keepOrder = keepOrder || !poly->texture;
		if (keepOrder)
		{
			// make skywalls and horizon lines first in order
			key->shader = -1;
			continue;
		}

		if (shaders)
			key->shader = poly->shader;
		// [ZE]: Ahead of the sky occluders, which are drawn first and whose depth would hide these
		if (poly->polyFlags & PF_BeforeSky)
			key->shader = -2;
		if (poly->texture)
			key->texture = poly->texture->downloaded; // there should be a opengl texture name here, usable for comparisons
		key->polyFlags = poly->polyFlags;
		key->polyColor = poly->surf.PolyColor.rgba;
		if (!shaders)
			continue;

		if (poly->brightmap)
			key->brightmap = poly->brightmap->downloaded;
		key->tintColor = poly->surf.TintColor.rgba;
		key->fadeColor = poly->surf.FadeColor.rgba;
		key->lightLevel = poly->surf.LightInfo.light_level;
		key->fadeStart = poly->surf.LightInfo.fade_start;
		key->fadeEnd = poly->surf.LightInfo.fade_end;
		key->directional = poly->surf.LightInfo.directional;
		key->lightTableId = poly->surf.LightTableId; // [ZE]: palette rendering
	}
}

// [ZE]: Binds a batch's texture and brightmap. The driver picks the texture unit from TF_BRIGHTMAP, and by now
// the flag says how the texture was last collected, which can be the other use (see HWR_GetTexture).
static void HWR_SetBatchTextures(GLMipmap_t *texture, GLMipmap_t *brightmap)
{
	if (texture)
		texture->flags &= ~TF_BRIGHTMAP;
	HWD.pfnSetTexture(texture);
	if (brightmap)
	{
		brightmap->flags |= TF_BRIGHTMAP;
		HWD.pfnSetTexture(brightmap);
	}
}

// This function organizes the geometry collected by HWR_ProcessPolygon calls into batches and uses
// the rendering backend to draw them.
void HWR_RenderBatches(void)
{
	int finalVertexWritePos = 0;// position in finalVertexArray
	int finalIndexWritePos = 0;// position in finalVertexIndexArray

	int polygonReadPos = 0;// position in polygonIndexArray

	int currentShader;
	int nextShader = 0;
	GLMipmap_t *currentTexture = NULL;
	GLMipmap_t *nextTexture = NULL;
	GLMipmap_t *currentBrightmap = NULL;
	GLMipmap_t *nextBrightmap = NULL;
	FBITFIELD currentPolyFlags = 0;
	FBITFIELD nextPolyFlags = 0;
	FSurfaceInfo currentSurfaceInfo;
	FSurfaceInfo nextSurfaceInfo;
	// [ZE]: The loop counter that filled polygonIndexArray went with that loop into sortPolygonIndices.

	if (!currently_batching)
		I_Error("HWR_RenderBatches called without starting batching");

	nextSurfaceInfo.LightInfo.fade_end = 0;
	nextSurfaceInfo.LightInfo.fade_start = 0;
	nextSurfaceInfo.LightInfo.light_level = 0;
	nextSurfaceInfo.LightInfo.directional = false;
	// [ZE]: Palette rendering's light table is part of the surface info too.
	nextSurfaceInfo.LightTableId = 0;

	currently_batching = false;// no longer collecting batches
	if (!polygonArraySize)
	{
		ps_hw_numpolys = ps_hw_numcalls = ps_hw_numshaders = ps_hw_numtextures = ps_hw_numpolyflags = ps_hw_numcolors = 0;
		return;// nothing to draw
	}
	// init stats vars
	ps_hw_numpolys = polygonArraySize;
	ps_hw_numcalls = ps_hw_numverts = 0;
	ps_hw_numshaders = ps_hw_numtextures = ps_hw_numpolyflags = ps_hw_numcolors = 1;
	// [ZE]: sort polygonIndexArray by the polygons' compact keys
	ps_hw_batchsorttime = I_GetPreciseTime();
	if (polygonSortKeysAllocSize < polygonArrayAllocSize)
	{
		free(polygonSortKeys);
		polygonSortKeys = malloc(polygonArrayAllocSize * sizeof(PolygonSortKey));
		polygonSortKeysAllocSize = polygonArrayAllocSize;
	}
	buildPolygonSortKeys(cv_glshaders.value && gl_shadersavailable);
	// [ZE]: Sort groups of equal keys, retaining submission order within each group.
	sortPolygonIndices(polygonIndexArray, polygonArraySize);
	ps_hw_batchsorttime = I_GetPreciseTime() - ps_hw_batchsorttime;
	// sort order
	// 1. shader
	// 2. texture
	// 3. brightmap
	// 4. polyflags
	// 5. colors + light level
	// not sure about what order of the last 2 should be, or if it even matters

	ps_hw_batchdrawtime = I_GetPreciseTime();

	currentShader = polygonArray[polygonIndexArray[0]].shader;
	currentTexture = polygonArray[polygonIndexArray[0]].texture;
	currentBrightmap = polygonArray[polygonIndexArray[0]].brightmap;
	currentPolyFlags = polygonArray[polygonIndexArray[0]].polyFlags;
	currentSurfaceInfo = polygonArray[polygonIndexArray[0]].surf;
	// For now, will sort and track the colors. Vertex attributes could be used instead of uniforms
	// and a color array could replace the color calls.

	// set state for first batch

	if (cv_glshaders.value && gl_shadersavailable)
	{
		HWD.pfnSetShader(currentShader);
	}

	if (currentPolyFlags & PF_NoTexture)
	{
		currentTexture = currentBrightmap = NULL;
	}
	else
	{
		// [ZE]: Bind the texture and brightmap, each with its own role.
		HWR_SetBatchTextures(currentTexture, currentBrightmap);
	}

	while (1)// note: remember handling notexture polyflag as having texture number 0 (also in comparePolygons)
	{
		int firstIndex;
		int lastIndex;

		boolean stopFlag = false;
		boolean changeState = false;
		boolean changeShader = false;
		boolean changeTexture = false;
		boolean changePolyFlags = false;
		boolean changeSurfaceInfo = false;

		// steps:
		// write vertices
		// check for changes or end, otherwise go back to writing
			// changes will affect the next vars and the change bools
			// end could set flag for stopping
		// execute draw call
		// could check ending flag here
		// change states according to next vars and change bools, updating the current vars and reseting the bools
		// reset write pos
		// repeat loop

		int index = polygonIndexArray[polygonReadPos++];
		int numVerts = polygonArray[index].numVerts;
		// before writing, check if there is enough room
		// using 'while' instead of 'if' here makes sure that there will *always* be enough room.
		// probably never will this loop run more than once though
		while (finalVertexWritePos + numVerts > finalVertexArrayAllocSize)
		{
			FOutVector* new_array;
			unsigned int* new_index_array;
			finalVertexArrayAllocSize *= 2;
			new_array = malloc(finalVertexArrayAllocSize * sizeof(FOutVector));
			memcpy(new_array, finalVertexArray, finalVertexWritePos * sizeof(FOutVector));
			free(finalVertexArray);
			finalVertexArray = new_array;
			// also increase size of index array, 3x of vertex array since
			// going from fans to triangles increases vertex count to 3x
			new_index_array = malloc(finalVertexArrayAllocSize * 3 * sizeof(UINT32));
			memcpy(new_index_array, finalVertexIndexArray, finalIndexWritePos * sizeof(UINT32));
			free(finalVertexIndexArray);
			finalVertexIndexArray = new_index_array;
		}
		// write the vertices of the polygon
		memcpy(&finalVertexArray[finalVertexWritePos], &unsortedVertexArray[polygonArray[index].vertsIndex],
			numVerts * sizeof(FOutVector));
		// write the indexes, pointing to the fan vertexes but in triangles format
		firstIndex = finalVertexWritePos;
		lastIndex = finalVertexWritePos + numVerts;
		finalVertexWritePos += 2;
		while (finalVertexWritePos < lastIndex)
		{
			finalVertexIndexArray[finalIndexWritePos++] = firstIndex;
			finalVertexIndexArray[finalIndexWritePos++] = finalVertexWritePos - 1;
			finalVertexIndexArray[finalIndexWritePos++] = finalVertexWritePos++;
		}

		if (polygonReadPos >= polygonArraySize)
		{
			stopFlag = true;
		}
		else
		{
			// check if a state change is required, set the change bools and next vars
			int nextIndex = polygonIndexArray[polygonReadPos];
			nextShader = polygonArray[nextIndex].shader;
			nextTexture = polygonArray[nextIndex].texture;
			nextBrightmap = polygonArray[nextIndex].brightmap;
			nextPolyFlags = polygonArray[nextIndex].polyFlags;
			nextSurfaceInfo = polygonArray[nextIndex].surf;
			if (nextPolyFlags & PF_NoTexture)
				nextTexture = nextBrightmap = 0;
			if (currentShader != nextShader && cv_glshaders.value && gl_shadersavailable)
			{
				changeState = true;
				changeShader = true;
			}
			if (currentTexture != nextTexture || currentBrightmap != nextBrightmap)
			{
				changeState = true;
				changeTexture = true;
			}
// [ZE]: Under VibeRant, a change in a roll sprite's flag or roll data starts a new state and surface info.
#ifdef HAVE_VIBERANT
			if (HWR_VibeRant() && (((currentPolyFlags ^ nextPolyFlags) & PF_RollSprite)
				|| ((currentPolyFlags & nextPolyFlags & PF_RollSprite)
				&& memcmp(currentSurfaceInfo.roll, nextSurfaceInfo.roll, sizeof(currentSurfaceInfo.roll)))))
				changeState = changeSurfaceInfo = true;
#endif
			if (currentPolyFlags != nextPolyFlags)
			{
				changeState = true;
				changePolyFlags = true;
			}
			if (cv_glshaders.value && gl_shadersavailable)
			{
				if (currentSurfaceInfo.PolyColor.rgba != nextSurfaceInfo.PolyColor.rgba ||
					currentSurfaceInfo.TintColor.rgba != nextSurfaceInfo.TintColor.rgba ||
					currentSurfaceInfo.FadeColor.rgba != nextSurfaceInfo.FadeColor.rgba ||
					currentSurfaceInfo.LightInfo.light_level != nextSurfaceInfo.LightInfo.light_level ||
					currentSurfaceInfo.LightInfo.fade_start != nextSurfaceInfo.LightInfo.fade_start ||
					currentSurfaceInfo.LightInfo.fade_end != nextSurfaceInfo.LightInfo.fade_end ||
					// [ZE]: Palette rendering's light table also splits a batch.
					currentSurfaceInfo.LightInfo.directional != nextSurfaceInfo.LightInfo.directional ||
					currentSurfaceInfo.LightTableId != nextSurfaceInfo.LightTableId)
				{
					changeState = true;
					changeSurfaceInfo = true;
				}
			}
			else
			{
				if (currentSurfaceInfo.PolyColor.rgba != nextSurfaceInfo.PolyColor.rgba)
				{
					changeState = true;
					changeSurfaceInfo = true;
				}
			}
		}

		if (changeState || stopFlag)
		{
			// execute draw call
			HWD.pfnDrawIndexedTriangles(&currentSurfaceInfo, finalVertexArray, finalIndexWritePos, currentPolyFlags, finalVertexIndexArray);
			// update stats
			ps_hw_numcalls++;
			ps_hw_numverts += finalIndexWritePos;
			// reset write positions
			finalVertexWritePos = 0;
			finalIndexWritePos = 0;
		}
		else continue;

		// if we're here then either its time to stop or time to change state
		if (stopFlag) break;

		// change state according to change bools and next vars, update current vars and reset bools
		if (changeShader)
		{
			HWD.pfnSetShader(nextShader);
			currentShader = nextShader;
			changeShader = false;

			ps_hw_numshaders++;
		}
		if (changeTexture)
		{
			// texture should be already ready for use from calls to SetTexture during batch collection
			// [ZE]: Bind the texture and brightmap, each with its own role.
			HWR_SetBatchTextures(nextTexture, nextBrightmap);
			// [ZE]: Upstream bound the brightmap here; HWR_SetBatchTextures does that now.
			currentTexture = nextTexture;
			currentBrightmap = nextBrightmap;

			changeTexture = false;
			ps_hw_numtextures++;
		}
		if (changePolyFlags)
		{
			currentPolyFlags = nextPolyFlags;
			changePolyFlags = false;

			ps_hw_numpolyflags++;
		}
		if (changeSurfaceInfo)
		{
			currentSurfaceInfo = nextSurfaceInfo;
			changeSurfaceInfo = false;

			ps_hw_numcolors++;
		}
		// and that should be it?
	}
	// reset the arrays (set sizes to 0)
	polygonArraySize = 0;
	unsortedVertexArraySize = 0;

	ps_hw_batchdrawtime = I_GetPreciseTime() - ps_hw_batchdrawtime;
}


#endif // HWRENDER
