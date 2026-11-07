#pragma once

#include <glib.h>

/* The marks git's machine output uses, shared by every reader of it */
#define FIELD_SEPARATOR "\x1f"
#define HEX_DIGITS "0123456789abcdef"
#define BRANCH_PREFIX "refs/heads/"
#define REMOTE_PREFIX "refs/remotes/"
#define TAG_PREFIX "refs/tags/"
/* The branch a remote points at by default, which is no branch of its own */
#define REMOTE_HEAD_SUFFIX "/HEAD"

/* Steps through output that git separates with NUL bytes, as -z gives it, returning FALSE after the last record */
gboolean read_next_record(const char *output, gsize length, gsize *position, const char **record,
	gsize *record_length);
