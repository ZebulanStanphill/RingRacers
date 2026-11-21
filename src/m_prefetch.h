// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  m_prefetch.h
/// \brief [ZE]: Hints that ask the CPU to start loading memory a loop is about to read
///
/// A prefetch has no effect a program can observe: it reads nothing into the program, writes nothing, and can't
/// fault, whatever address it is given. It only starts loading a cache line early. So adding one never changes what
/// the game does, only how long a later read of that memory waits. On an Xbox One X's Jaguar cores a read of memory
/// that isn't cached stalls for about a hundred nanoseconds, and loops that go from object to object (a blockmap
/// cell's things, the thinker list) pay that for every object, one stall after another, because the address of the
/// next object is only known once the current one has been read.

#ifndef __M_PREFETCH__
#define __M_PREFETCH__

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define M_Prefetch(address) __builtin_prefetch((const void *)(address))
#else
#define M_Prefetch(address) ((void)(address))
#endif

#define M_CACHELINE 64

// The hints below are calls to small inline functions, and the tests and loops they need are inside those functions,
// on purpose: the Xbox build's profile-guided optimization matches a function to its recorded profile by the shape
// of its control flow, so a hint that added an if or a loop to a hot function would cost that function its profile.
// A call adds nothing to that shape.

// One cache line at an offset into an object. NULL is fine: nothing is prefetched.
static inline void M_PrefetchAt(const void *object, size_t offset)
{
	if (object)
		M_Prefetch((const char *)object + offset);
}

// The first bytes of an object, a cache line at a time. NULL is fine.
static inline void M_PrefetchBytes(const void *object, size_t bytes)
{
	size_t offset;

	if (!object)
		return;

	for (offset = 0; offset < bytes; offset += M_CACHELINE)
		M_Prefetch((const char *)object + offset);
}

// The cache lines a walk over a blockmap cell's things reads of each thing: its thinker function (removed or not)
// and position, its bnext link, and its radius. An expression, with no control flow of its own.
#define M_PrefetchBlockThing(mobj) \
	(M_PrefetchAt((mobj), 0), M_PrefetchAt((mobj), offsetof(mobj_t, bnext)), M_PrefetchAt((mobj), offsetof(mobj_t, radius)))

#endif // __M_PREFETCH__
