#include <string.h>

#include "git/word-diff.h"

/* Longer lines take too long to compare, and are usually data rather than something a person reads word by word */
#define MAXIMUM_TOKENS 300
/* Below this share of unchanged text, marking words only adds noise to a line that was rewritten */
#define MINIMUM_SIMILARITY 0.35
#define FIRST_UTF8_BYTE 0x80


static gboolean is_word_byte(unsigned char byte) {
	return g_ascii_isalnum(byte) || byte == '_' || byte >= FIRST_UTF8_BYTE;
}


/* Words, runs of spaces and single other characters, each as its start in the text */
static GArray *split_tokens(const char *text) {
	GArray *starts = g_array_new(FALSE, FALSE, sizeof(int));
	int position = 0;
	while (text[position]) {
		g_array_append_val(starts, position);
		unsigned char first = (unsigned char)text[position];
		if (is_word_byte(first))
			while (text[position] && is_word_byte((unsigned char)text[position]))
				position++;
		else if (g_ascii_isspace(first))
			while (text[position] && g_ascii_isspace(text[position]))
				position++;
		else
			position++;
	}
	int end = position;
	g_array_append_val(starts, end);
	return starts;
}


static gboolean are_tokens_equal(const char *old_text, const GArray *old_starts, int old_index,
	const char *new_text, const GArray *new_starts, int new_index) {
	int old_start = g_array_index(old_starts, int, old_index);
	int old_length = g_array_index(old_starts, int, old_index + 1) - old_start;
	int new_start = g_array_index(new_starts, int, new_index);
	int new_length = g_array_index(new_starts, int, new_index + 1) - new_start;
	return old_length == new_length && memcmp(old_text + old_start, new_text + new_start, old_length) == 0;
}


/* Adds the token to the ranges, joining it to the last range when they touch */
static void add_range(GArray *ranges, int start, int end) {
	if (ranges->len > 0) {
		ByteRange *last = &g_array_index(ranges, ByteRange, ranges->len - 1);
		if (last->end == start) {
			last->end = end;
			return;
		}
	}
	ByteRange range = {start, end};
	g_array_append_val(ranges, range);
}


/* The longest common run of tokens, walked back to mark every token outside it as changed */
gboolean find_changed_words(const char *old_text, const char *new_text, GArray *old_ranges, GArray *new_ranges) {
	g_autoptr(GArray) old_starts = split_tokens(old_text);
	g_autoptr(GArray) new_starts = split_tokens(new_text);
	int old_count = (int)old_starts->len - 1, new_count = (int)new_starts->len - 1;
	if (old_count == 0 || new_count == 0 || old_count > MAXIMUM_TOKENS || new_count > MAXIMUM_TOKENS)
		return FALSE;
	int width = new_count + 1;
	g_autofree int *lengths = g_new0(int, (gsize)(old_count + 1) * (gsize)width);
	for (int old_index = old_count - 1; old_index >= 0; old_index--)
		for (int new_index = new_count - 1; new_index >= 0; new_index--)
			lengths[old_index * width + new_index] = are_tokens_equal(old_text, old_starts, old_index, new_text,
				new_starts, new_index) ? lengths[(old_index + 1) * width + new_index + 1] + 1 :
				MAX(lengths[(old_index + 1) * width + new_index], lengths[old_index * width + new_index + 1]);
	int kept_bytes = 0;
	int old_index = 0, new_index = 0;
	while (old_index < old_count || new_index < new_count) {
		if (old_index < old_count && new_index < new_count &&
			are_tokens_equal(old_text, old_starts, old_index, new_text, new_starts, new_index)) {
			kept_bytes += g_array_index(old_starts, int, old_index + 1) - g_array_index(old_starts, int, old_index);
			old_index++, new_index++;
		} else if (new_index < new_count && (old_index == old_count ||
			lengths[old_index * width + new_index + 1] >= lengths[(old_index + 1) * width + new_index])) {
			add_range(new_ranges, g_array_index(new_starts, int, new_index), g_array_index(new_starts, int, new_index + 1));
			new_index++;
		} else {
			add_range(old_ranges, g_array_index(old_starts, int, old_index), g_array_index(old_starts, int, old_index + 1));
			old_index++;
		}
	}
	int longest = MAX((int)strlen(old_text), (int)strlen(new_text));
	if (longest == 0 || (double)kept_bytes / longest < MINIMUM_SIMILARITY) {
		g_array_set_size(old_ranges, 0);
		g_array_set_size(new_ranges, 0);
		return FALSE;
	}
	return TRUE;
}


GArray *pair_replaced_lines(const DiffHunk *hunk) {
	guint count = hunk->lines->len;
	GArray *partners = g_array_sized_new(FALSE, FALSE, sizeof(int), count);
	int none = -1;
	for (guint index = 0; index < count; index++)
		g_array_append_val(partners, none);
	guint index = 0;
	while (index < count) {
		DiffLine *line = g_ptr_array_index(hunk->lines, index);
		if (line->kind != DIFF_LINE_REMOVED) {
			index++;
			continue;
		}
		guint removed_start = index;
		while (index < count && ((DiffLine *)g_ptr_array_index(hunk->lines, index))->kind == DIFF_LINE_REMOVED)
			index++;
		guint added_start = index;
		while (index < count && ((DiffLine *)g_ptr_array_index(hunk->lines, index))->kind == DIFF_LINE_ADDED)
			index++;
		guint pairs = MIN(added_start - removed_start, index - added_start);
		for (guint pair = 0; pair < pairs; pair++) {
			g_array_index(partners, int, removed_start + pair) = (int)(added_start + pair);
			g_array_index(partners, int, added_start + pair) = (int)(removed_start + pair);
		}
	}
	return partners;
}
