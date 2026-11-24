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
/// that isn't cached stalls for about a hundred nanoseconds, and a loop that goes from object to object (the thinker
/// list) pays that for every object, one stall after another, because the address of the next object is only known
/// once the current one has been read. A hint pays off where the loop does enough work on each object for the next
/// one's memory to arrive meanwhile: it did for the thinker list, and measurably didn't for the blockmap thing walk
/// or the interpolation loop (see CHANGES.md). The hardware renderer's level walk also asks for several objects whose
/// addresses it already knows (a subsector's segs, then their linedefs) together, so that they load side by side
/// instead of one stall after another.

#ifndef __M_PREFETCH__
#define __M_PREFETCH__

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define M_Prefetch(address) __builtin_prefetch((const void *)(address))
#else
#define M_Prefetch(address) ((void)(address))
#endif

#define M_CACHELINE 64

// A hint is a call to a small inline function, and the tests and loops it needs are inside that function, on
// purpose: the Xbox build's profile-guided optimization matches a function to its recorded profile by the shape
// of its control flow, so a hint that added an if or a loop to a hot function would cost that function its profile.
// A call adds nothing to that shape.

// The first bytes of an object, a cache line at a time. NULL is fine.
static inline void M_PrefetchBytes(const void *object, size_t bytes)
{
	size_t offset;

	if (!object)
		return;

	for (offset = 0; offset < bytes; offset += M_CACHELINE)
		M_Prefetch((const char *)object + offset);
}

// Every cache line that holds any of an object's first bytes, wherever in its line the object starts. NULL is fine.
static inline void M_PrefetchSpan(const void *object, size_t bytes)
{
	const uintptr_t mask = ~(uintptr_t)(M_CACHELINE - 1);
	uintptr_t line, last;
	size_t lines;

	if (!object || !bytes)
		return;

	line = (uintptr_t)object & mask;
	last = ((uintptr_t)object + bytes - 1) & mask;
	if (last < line) // the span would wrap around the address space
		return;

	// Counted, so that a span ending in the last line of the address space still ends.
	for (lines = (last - line) / M_CACHELINE + 1; lines > 0; lines--, line += M_CACHELINE)
		M_Prefetch(line);
}

#endif // __M_PREFETCH__
