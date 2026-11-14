// This fork's own version text; see fork_version.h.

#include "doomdef.h"
#include "fork_version.h"

const char *ForkVersion_String(void)
{
	static char text[64];

	if (text[0] == '\0')
		ForkVersion_Format(text, sizeof text, SRB2VERSION, FORKVERSION, compforkrelease, comprevision);

	return text;
}
