// This fork's own version, shown on top of the Ring Racers version.
//
// Display and log only: nothing here is sent over the network or written to
// replays or saves, so it cannot affect compatibility with the unmodified game.
// See "Versioning" in AGENTS.md.

#ifndef __FORK_VERSION_H__
#define __FORK_VERSION_H__

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// The fork's own release number on this Ring Racers version, as in
// <SRB2VERSION>_Z<FORKVERSION> (2.4_Z1, 2.4_Z1.1, 2.4.1_Z1). "0" means no
// release has been made on this Ring Racers version yet. A release commit
// raises it and is tagged.
#define FORKVERSION "0"

// Writes the version text for display into buf (size bytes, always
// NUL-terminated, truncated if too small):
//   release:      v2.4_Z1
//   development:  v2.4_Z1-dev (1a2b3c4d)
// The parenthesised part is the first 8 characters of the commit hash; it is
// left out when revision is empty or not a hash (upstream's fallbacks are
// "illegal" and "Unknown").
static inline void ForkVersion_Format(char *buf, size_t size, const char *base,
	const char *fork, int release, const char *revision)
{
	size_t i, n = 0;
	int ishash = 1;

	if (size == 0)
		return;

	if (revision != NULL)
	{
		n = strlen(revision);
		for (i = 0; i < n; i++)
		{
			char c = revision[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			{
				ishash = 0;
				break;
			}
		}
	}
	if (n == 0)
		ishash = 0;

	if (release)
		snprintf(buf, size, "v%s_Z%s", base, fork);
	else if (ishash)
		snprintf(buf, size, "v%s_Z%s-dev (%.8s)", base, fork, revision);
	else
		snprintf(buf, size, "v%s_Z%s-dev", base, fork);
}

// The version text for this build (built once). Defined in fork_version.c.
const char *ForkVersion_String(void);

#ifdef __cplusplus
}
#endif

#endif
