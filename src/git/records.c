#include <string.h>

#include "git/records.h"


gboolean read_next_record(const char *output, gsize length, gsize *position, const char **record,
	gsize *record_length) {
	if (*position >= length)
		return FALSE;
	*record = output + *position;
	const char *end = memchr(*record, 0, length - *position);
	*record_length = end ? (gsize)(end - *record) : length - *position;
	*position += *record_length + 1;
	return TRUE;
}
