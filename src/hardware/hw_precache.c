// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  hw_precache.c
/// \brief [UWP]: Warms sprite textures while a level loads, so Legacy GL doesn't create them mid-race.

#include "../doomdef.h"

#ifdef _UWP

#include "../doomstat.h"
#include "../i_system.h"
#include "../i_video.h"
#include "../info.h"
#include "../p_local.h"
#include "../d_player.h"
#include "../g_game.h" // players, playeringame
#include "../p_mobj.h"
#include "../p_tick.h"
#include "../r_defs.h"
#include "../r_picformats.h" // spriteinfo
#include "../r_draw.h"
#include "../r_skins.h"
#include "../r_state.h"
#include "../r_things.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "hw_glob.h"
#include "hw_main.h"
#include "hw_md2.h"

// A race holds about 700-800 MB of textures after a few minutes, and the D3D12 driver flushes
// every texture when usage passes the video memory budget. Stop warming once sprites have added
// this much, so a lobby of many skins can't push the budget; what's left stays lazy.
#define SPRITEWARM_MAXBYTES (160 * 1024 * 1024)
// Check the driver's texture total every this many patches (it walks every texture).
#define SPRITEWARM_CHECKEVERY 16
// Distinct (sprite, colormap) pairs remembered so a sprite isn't visited twice. Past this,
// visits repeat; a repeat finds the texture cached, so it's only slower, never wrong.
#define SPRITEWARM_MAXSEEN 4096
// Distinct (type, color, flags) level thing variants remembered per call.
#define SPRITEWARM_MAXVARIANTS 1024

typedef struct
{
	const void *sprite; // spritedef_t of the sprite, or of the skin's sprite2
	const UINT8 *colormap;
} spritewarm_seen_t;

typedef struct
{
	INT32 frames, textures;
	UINT32 uploads0;
	INT32 used0;
	INT32 sincecheck;
	boolean full;
	spritewarm_seen_t *seen;
	size_t numseen;
} spritewarm_t;

// Mobj types every race spawns, or spawns routinely, whether or not the level places one at load:
// items and what they spawn, the item boxes, rings, kart effects, impacts and explosions.
static const mobjtype_t spritewarm_common[] =
{
	MT_RING, MT_FLINGRING, MT_BLUESPHERE, MT_RANDOMITEM, MT_SPHEREBOX, MT_FLOATINGITEM,
	MT_ITEMCAPSULE, MT_ITEMCAPSULE_PART, MT_MONITOR, MT_MONITOR_PART, MT_MONITOR_SHARD,
	MT_ITEM_DEBRIS, MT_ITEM_DEBRIS_CLOUD_SPAWNER,
	MT_BANANA, MT_BANANA_SHIELD, MT_BANANA_SPARK, MT_ORBINAUT, MT_ORBINAUT_SHIELD,
	MT_JAWZ, MT_JAWZ_SHIELD, MT_SSMINE, MT_SSMINE_SHIELD, MT_LANDMINE,
	MT_EGGMANITEM, MT_EGGMANITEM_SHIELD, MT_DROPTARGET, MT_DROPTARGET_SHIELD,
	MT_BALLHOG, MT_BALLHOGBOOM, MT_SPB, MT_SPBEXPLOSION, MT_ROCKETSNEAKER,
	MT_LIGHTNINGSHIELD, MT_LIGHTNINGSHIELD_VISUAL, MT_LIGHTNINGATTACK_VISUAL,
	MT_BUBBLESHIELD, MT_BUBBLESHIELD_VISUAL, MT_FLAMESHIELD, MT_FLAMESHIELD_VISUAL,
	MT_FLAMESHIELDUNDERLAY, MT_FLAMESHIELDPAPER, MT_GARDENTOP, MT_GARDENTOPSPARK,
	MT_HYUDORO, MT_HYUDORO_CENTER, MT_GROW_PARTICLE, MT_SHRINK_PARTICLE,
	MT_SINK, MT_SINK_SHIELD, MT_SINKTRAIL, MT_GACHABOM, MT_DUELBOMB,
	MT_PLAYERRETICULE, MT_INSTAWHIP, MT_INSTASHIELDA, MT_INSTASHIELDB, MT_POWERCLASH,
	MT_ITEMCLASH, MT_FIREDITEM, MT_GUARDBREAK, MT_BUMP,
	MT_BOOMEXPLODE, MT_BOOMPARTICLE, MT_SMOLDERING, MT_EXPLODE, MT_UWEXPLODE, MT_SPARK, MT_DUST,
	MT_FASTLINE, MT_FASTDUST, MT_DRIFTDUST, MT_DRIFTSPARK, MT_DRIFTEXPLODE, MT_DRIFTCLIP,
	MT_DRIFTCLIPSPARK, MT_DRIFTELECTRICITY, MT_DRIFTELECTRICSPARK, MT_BRAKEDRIFT, MT_BRAKEDUST,
	MT_BOOSTFLAME, MT_BOOSTSMOKE, MT_SNEAKERTRAIL, MT_SPARKLETRAIL, MT_INVULNFLASH,
	MT_WIPEOUTTRAIL, MT_SPINDASHDUST, MT_SPINDASHWIND, MT_SOFTLANDING, MT_SMOOTHLANDING,
	MT_DRAFTDUST, MT_SPBDUST, MT_TIREGREASE, MT_WATERTRAIL, MT_WATERTRAILUNDERLAY,
	MT_SPLISH, MT_SMALLBUBBLE, MT_MEDIUMBUBBLE,
	MT_TRICKINDICATOR, MT_SIDETRICK, MT_FORWARDTRICK, MT_WAVEDASH, MT_AMPRING, MT_AMPBODY,
	MT_AMPAURA, MT_AMPBURST, MT_GOTIT, MT_SIGNSPARKLE, MT_RINGSPARKS, MT_KART_LEFTOVER,
	MT_KART_TIRE, MT_KART_PARTICLE, MT_BATTLEBUMPER, MT_BATTLEBUMPER_DEBRIS, MT_BATTLEBUMPER_BLAST,
	MT_BATTLEPOINT, MT_KARMAHITBOX, MT_KARMAWHEEL, MT_FZEROBOOM,
	MT_DASHRING, MT_RAINBOWDASHRING, MT_SNEAKERPANEL,
	MT_YELLOWSPRING, MT_REDSPRING, MT_BLUESPRING, MT_GREYSPRING,
	MT_YELLOWDIAG, MT_REDDIAG, MT_BLUEDIAG, MT_GREYDIAG,
	MT_YELLOWHORIZ, MT_REDHORIZ, MT_BLUEHORIZ, MT_GREYHORIZ,
	MT_PAPERITEMSPOT, MT_THOK,
};

// Mobj types a kart spawns in its player's color: what the player's mobj color selects, so the
// translation is the one for that color and TC_DEFAULT, or TC_RAINBOW when the spawner colorizes.
static const struct { mobjtype_t type; boolean colorized; } spritewarm_player[] =
{
	{MT_FASTLINE, true}, {MT_SPARKLETRAIL, false}, {MT_BLOCKRING, false}, {MT_BLOCKBODY, false},
	{MT_BATTLEBUMPER, false}, {MT_BATTLEPOINT, false},
};

// Same selection as HWR_ProjectSprite for a mobj that isn't a skin's player: an explicit color,
// else the default colormap, which is shifted to Encore's remap unless the mobj opts out.
static const UINT8 *SpriteWarm_Colormap(UINT16 color, boolean colorized, UINT32 flags)
{
	const UINT8 *colormap;
	if (color)
		return R_GetTranslationColormap(colorized ? TC_RAINBOW : TC_DEFAULT, (skincolornum_t)color, GTC_CACHE);
	colormap = colormaps;
	if (encoremap && !(flags & MF_DONTENCOREMAP))
		colormap += COLORMAP_REMAPOFFSET;
	return colormap;
}

static boolean SpriteWarm_Seen(spritewarm_t *w, const void *sprite, const UINT8 *colormap)
{
	size_t i;
	for (i = 0; i < w->numseen; i++)
		if (w->seen[i].sprite == sprite && w->seen[i].colormap == colormap)
			return true;
	if (w->numseen < SPRITEWARM_MAXSEEN)
	{
		w->seen[w->numseen].sprite = sprite;
		w->seen[w->numseen].colormap = colormap;
		w->numseen++;
	}
	return false;
}

// Create one patch's texture the way the sprite draw path does: HWR_GetMappedPatch with the
// vissprite's colormap (which keys the cached texture by that colormap's pointer), and the
// brightmap texture when it matches the patch in size.
static void SpriteWarm_Patch(spritewarm_t *w, lumpnum_t lump, const spriteinfo_t *sprinfo, UINT8 frame, const UINT8 *colormap)
{
	patch_t *patch = (patch_t *)W_CachePatchNum(lump, PU_SPRITE);
	patch_t *bright = R_CacheSpriteBrightMap(sprinfo, frame);
	HWR_GetMappedPatch(patch, colormap);
	if (bright && bright->width == patch->width && bright->height == patch->height)
		HWR_GetSpriteBrightmap(bright);
	HWR_UWPLoadKeepAlive();
	if (++w->sincecheck >= SPRITEWARM_CHECKEVERY)
	{
		w->sincecheck = 0;
		if ((INT64)HWR_GetTextureUsed() - w->used0 >= SPRITEWARM_MAXBYTES)
			w->full = true;
	}
}

// Every rotation HWR_ProjectSprite can pick for a frame: slot 0 for a single patch, else
// eight view angles, or sixteen for 3DGE-style frames.
static void SpriteWarm_Frame(spritewarm_t *w, const spritedef_t *sprdef, const spriteinfo_t *sprinfo, size_t frame, const UINT8 *colormap)
{
	const spriteframe_t *sprframe = &sprdef->spriteframes[frame];
	const INT32 slots = sprframe->rotate == SRF_SINGLE ? 1 : (sprframe->rotate & SRF_3DGE) ? 16 : 8;
	INT32 r, i;
	if (sprframe->rotate == SRF_NONE)
		return;
	w->frames++;
	for (r = 0; r < slots && !w->full; r++)
	{
		if (sprframe->lumppat[r] == LUMPERROR)
			continue;
		for (i = 0; i < r; i++) // 2D sprites repeat one patch in many slots
			if (sprframe->lumppat[i] == sprframe->lumppat[r])
				break;
		if (i < r)
			continue;
		SpriteWarm_Patch(w, sprframe->lumppat[r], sprinfo, (UINT8)frame, colormap);
	}
}

static void SpriteWarm_Def(spritewarm_t *w, const spritedef_t *sprdef, const spriteinfo_t *sprinfo, const UINT8 *colormap)
{
	size_t f;
	if (!sprdef->numframes || !sprdef->spriteframes || SpriteWarm_Seen(w, sprdef, colormap))
		return;
	for (f = 0; f < sprdef->numframes && !w->full; f++)
		SpriteWarm_Frame(w, sprdef, &sprinfo[0], f, colormap);
}

// A sprite whose Legacy GL model loaded is drawn as that model, never as a sprite (the model
// precache has already made its texture). Anything else is drawn from its patches.
static void SpriteWarm_Sprite(spritewarm_t *w, spritenum_t spr, const UINT8 *colormap)
{
	if ((unsigned)spr >= numsprites || spr == SPR_NONE || spr == SPR_PLAY)
		return;
	if (cv_glmodels.value && !md2_models[spr].notfound && !md2_models[spr].error)
		return;
	SpriteWarm_Def(w, &sprites[spr], &spriteinfo[spr], colormap);
}

// Warm the sprite of every state reachable from `state` by nextstate.
static void SpriteWarm_Chain(spritewarm_t *w, statenum_t state, UINT8 *stateseen, const UINT8 *colormap)
{
	while (state != S_NULL && (unsigned)state < NUMSTATES && !stateseen[state] && !w->full)
	{
		stateseen[state] = 1;
		SpriteWarm_Sprite(w, states[state].sprite, colormap);
		state = states[state].nextstate;
	}
}

// The states a type enters on its own, or that its mobjinfo names for the engine to enter.
static void SpriteWarm_Type(spritewarm_t *w, mobjtype_t type, statenum_t current, const UINT8 *colormap, UINT8 *stateseen)
{
	const mobjinfo_t *info;
	if ((unsigned)type >= NUMMOBJTYPES)
		return;
	info = &mobjinfo[type];
	memset(stateseen, 0, NUMSTATES);
	SpriteWarm_Chain(w, current, stateseen, colormap);
	SpriteWarm_Chain(w, info->spawnstate, stateseen, colormap);
	SpriteWarm_Chain(w, info->seestate, stateseen, colormap);
	SpriteWarm_Chain(w, info->painstate, stateseen, colormap);
	SpriteWarm_Chain(w, info->meleestate, stateseen, colormap);
	SpriteWarm_Chain(w, info->missilestate, stateseen, colormap);
	SpriteWarm_Chain(w, info->deathstate, stateseen, colormap);
	SpriteWarm_Chain(w, info->xdeathstate, stateseen, colormap);
	SpriteWarm_Chain(w, info->raisestate, stateseen, colormap);
}

typedef struct
{
	mobjtype_t type;
	UINT16 color;
	UINT8 flags; // 1: colorized, 2: opts out of Encore's remap
} spritewarm_variant_t;

static boolean SpriteWarm_NewVariant(spritewarm_variant_t *list, size_t *count, mobjtype_t type, UINT16 color, UINT8 flags)
{
	size_t i;
	for (i = 0; i < *count; i++)
		if (list[i].type == type && list[i].color == color && list[i].flags == flags)
			return false;
	if (*count >= SPRITEWARM_MAXVARIANTS)
		return false;
	list[*count].type = type;
	list[*count].color = color;
	list[*count].flags = flags;
	(*count)++;
	return true;
}

// Level load: the sprites of every thing the map placed, and of what a race spawns
// routinely, in the colormap each is drawn with.
static void SpriteWarm_Level(spritewarm_t *w, UINT8 *stateseen)
{
	spritewarm_variant_t *variants = Z_Calloc(SPRITEWARM_MAXVARIANTS * sizeof *variants, PU_STATIC, NULL);
	size_t numvariants = 0, i;
	thinker_t *th;

	// Inspect the list; never execute thinkers, project sprites or change mobjs.
	for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ] && !w->full; th = th->next)
	{
		mobj_t *mo = (mobj_t *)th;
		UINT8 flags;
		if (P_MobjWasRemoved(mo) || mo->sprite == SPR_PLAY)
			continue;
		flags = (mo->colorized ? 1 : 0) | ((mo->flags & MF_DONTENCOREMAP) ? 2 : 0);
		if (!SpriteWarm_NewVariant(variants, &numvariants, mo->type, mo->color, flags))
			continue;
		SpriteWarm_Type(w, mo->type, mo->state ? (statenum_t)(mo->state - states) : S_NULL,
			SpriteWarm_Colormap(mo->color, mo->colorized, mo->flags), stateseen);
	}

	for (i = 0; i < sizeof spritewarm_common / sizeof *spritewarm_common && !w->full; i++)
	{
		const mobjtype_t type = spritewarm_common[i];
		SpriteWarm_Type(w, type, S_NULL, SpriteWarm_Colormap(0, false, mobjinfo[type].flags), stateseen);
	}
	Z_Free(variants);
}

// After P_InitPlayers: each player's kart sprites in the colormap its mobj draws with, and the
// effects it spawns in its color. Players of one skin and color share textures.
static void SpriteWarm_Players(spritewarm_t *w, UINT8 *stateseen)
{
	struct { skin_t *skin; const UINT8 *colormap; } karts[MAXPLAYERS];
	UINT16 colors[MAXPLAYERS];
	size_t numkarts = 0, numcolors = 0, i, j;
	UINT8 usedspr2[256];
	INT32 spr2;

	for (i = 0; i < MAXPLAYERS; i++)
	{
		mobj_t *mo;
		skin_t *skin;
		const UINT8 *colormap;
		if (!playeringame[i])
			continue;
		mo = players[i].mo;
		if (!mo || P_MobjWasRemoved(mo) || mo->sprite != SPR_PLAY || !mo->skin)
			continue;
		skin = (skin_t *)mo->skin;
		// Draws as a model, not as these sprites (the model precache made its texture).
		if (cv_glmodels.value && skin->skinnum < MAXSKINS
			&& !md2_playermodels[skin->skinnum].notfound && !md2_playermodels[skin->skinnum].error)
			continue;
		// Same translation as HWR_ProjectSprite; hidden skins in demos and hitlag flashes stay lazy.
		if (!R_CanShowSkinInDemo(skin->skinnum))
			continue;
		if (mo->color)
			colormap = R_GetTranslationColormap(mo->colorized ? TC_RAINBOW : skin->skinnum, (skincolornum_t)mo->color, GTC_CACHE);
		else
			colormap = SpriteWarm_Colormap(0, false, mo->flags);
		for (j = 0; j < numkarts; j++)
			if (karts[j].skin == skin && karts[j].colormap == colormap)
				break;
		if (j == numkarts)
		{
			karts[numkarts].skin = skin;
			karts[numkarts].colormap = colormap;
			numkarts++;
		}
		for (j = 0; j < numcolors; j++)
			if (colors[j] == mo->color)
				break;
		if (j == numcolors && mo->color)
			colors[numcolors++] = mo->color;
	}

	// Every sprite2 the kart states use, which is what P_SetPlayerMobjState resolves per skin.
	memset(usedspr2, 0, sizeof usedspr2);
	for (i = 0; i < NUMSTATES; i++)
		if (states[i].sprite == SPR_PLAY)
			usedspr2[states[i].frame & FF_FRAMEMASK] = 1;

	// Driving frames come first in the enum, and every player gets each before the next is
	// tried, so if the budget stops this early nobody is missing their normal driving frames.
	for (spr2 = 0; spr2 < 256 && !w->full; spr2++)
	{
		if (!usedspr2[spr2])
			continue;
		for (i = 0; i < numkarts && !w->full; i++)
		{
			skin_t *skin = karts[i].skin;
			const UINT8 resolved = P_GetSkinSprite2(skin, (UINT8)spr2, NULL);
			SpriteWarm_Def(w, &skin->sprites[resolved], &skin->sprinfo[resolved], karts[i].colormap);
		}
	}

	for (i = 0; i < numcolors && !w->full; i++)
		for (j = 0; j < sizeof spritewarm_player / sizeof *spritewarm_player && !w->full; j++)
			SpriteWarm_Type(w, spritewarm_player[j].type, S_NULL,
				SpriteWarm_Colormap(colors[i], spritewarm_player[j].colorized, mobjinfo[spritewarm_player[j].type].flags),
				stateseen);
}

void HWR_UWPPrecacheSprites(boolean players_only)
{
	spritewarm_t w;
	UINT8 *stateseen;
	precise_t start = I_GetPreciseTime();

	memset(&w, 0, sizeof w);
	w.seen = Z_Malloc(SPRITEWARM_MAXSEEN * sizeof *w.seen, PU_STATIC, NULL);
	stateseen = Z_Malloc(NUMSTATES, PU_STATIC, NULL);
	w.uploads0 = HWR_UWPPatchUploads();
	w.used0 = HWR_GetTextureUsed();

	if (players_only)
		SpriteWarm_Players(&w, stateseen);
	else
		SpriteWarm_Level(&w, stateseen);
	w.textures = (INT32)(HWR_UWPPatchUploads() - w.uploads0);

	Z_Free(stateseen);
	Z_Free(w.seen);
	I_OutputMsg("UWP: precache: sprites: %d frames, %d textures created in %.2f s (%s, %.1f MB%s)\n",
		w.frames, w.textures, (I_GetPreciseTime() - start) / (double)I_GetPrecisePrecision(),
		players_only ? "players" : "level",
		((INT64)HWR_GetTextureUsed() - w.used0) / 1048576.0,
		w.full ? ", stopped at the memory limit" : "");
}

#else
typedef int hw_precache_unused; // An empty translation unit isn't valid C.
#endif
