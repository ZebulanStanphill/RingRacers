/* The fork's version text formatting (src/fork_version.h); runs without the engine. */
#include "fork_version.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The header is C and C++; it is compiled as C++ here and as C in the engine. */

#define CHECK_FMT(expected, base, fork, release, rev) \
	do \
	{ \
		char buf[64]; \
		ForkVersion_Format(buf, sizeof buf, base, fork, release, rev); \
		if (strcmp(buf, expected) != 0) \
		{ \
			fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, expected, buf); \
			return 1; \
		} \
	} while (0)

int main(void)
{
	char buf[64];

	/* Releases ignore the revision. */
	CHECK_FMT("v2.4_Z1", "2.4", "1", 1, "1a2b3c4d5e6f");
	CHECK_FMT("v2.4_Z1", "2.4", "1", 1, "illegal");
	CHECK_FMT("v2.4_Z1.1", "2.4", "1.1", 1, NULL);
	CHECK_FMT("v2.4.1_Z1", "2.4.1", "1", 1, "");

	/* Development builds name the first 8 characters of the commit. */
	CHECK_FMT("v2.4_Z1-dev (1a2b3c4d)", "2.4", "1", 0, "1a2b3c4d5e6f7a8b");
	CHECK_FMT("v2.4_Z1-dev (1a2b3c4d)", "2.4", "1", 0, "1a2b3c4d");
	CHECK_FMT("v2.4_Z1-dev (1a2b3c4)", "2.4", "1", 0, "1a2b3c4"); /* short hash as is */
	CHECK_FMT("v2.4_Z1-dev (1A2B3C4D)", "2.4", "1", 0, "1A2B3C4D5E");
	CHECK_FMT("v2.4_Z0-dev (1a2b3c4d)", "2.4", "0", 0, "1a2b3c4d5e6f");
	CHECK_FMT("v2.4.1_Z1.1-dev (0123abcd)", "2.4.1", "1.1", 0, "0123abcdef");

	/* Not a commit: just -dev. */
	CHECK_FMT("v2.4_Z1-dev", "2.4", "1", 0, "");
	CHECK_FMT("v2.4_Z1-dev", "2.4", "1", 0, NULL);
	CHECK_FMT("v2.4_Z1-dev", "2.4", "1", 0, "illegal");
	CHECK_FMT("v2.4_Z0-dev", "2.4", "0", 0, "Unknown");
	CHECK_FMT("v2.4_Z1-dev", "2.4", "1", 0, "1a2b3c4g");

	/* Truncation: always terminated, never written past the buffer. */
	memset(buf, 'x', sizeof buf);
	ForkVersion_Format(buf, 8, "2.4", "1", 0, "1a2b3c4d5e");
	assert(strcmp(buf, "v2.4_Z1") == 0);
	assert(buf[8] == 'x');
	memset(buf, 'x', sizeof buf);
	ForkVersion_Format(buf, 5, "2.4", "1", 1, NULL);
	assert(strcmp(buf, "v2.4") == 0);
	assert(buf[5] == 'x');
	memset(buf, 'x', sizeof buf);
	ForkVersion_Format(buf, 1, "2.4", "1", 1, NULL);
	assert(buf[0] == '\0');
	assert(buf[1] == 'x');
	memset(buf, 'x', sizeof buf);
	ForkVersion_Format(buf, 0, "2.4", "1", 1, NULL);
	assert(buf[0] == 'x');

	/* The shipped fork version is a plain release number. */
	assert(FORKVERSION[0] >= '0' && FORKVERSION[0] <= '9');

	puts("fork_version: ok");
	return 0;
}
