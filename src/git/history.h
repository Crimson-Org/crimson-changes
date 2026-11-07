#pragma once

#include <gio/gio.h>

/* Reads a git log while the user scrolls, so a history of a million commits costs only what is in view: the log
 * runs once and its output is read in batches, and git waits on the full pipe between them */
typedef struct HistoryStream HistoryStream;

/* Called with a batch of Commit items, which the callee takes, then once with NULL after the last commit */
typedef void (*CommitsRead)(GPtrArray *commits, gpointer data);

/* Starts `git log` in the folder with the extra arguments, such as a search, and reads the first batch */
HistoryStream *start_history_stream(const char *folder, const char *const *extra_arguments, CommitsRead read,
	gpointer data);
/* Reads until at least this many commits arrived in all, or the log ended */
void want_history_commits(HistoryStream *stream, guint count);
guint get_history_commit_count(HistoryStream *stream);
gboolean has_history_ended(HistoryStream *stream);
/* Stops reading and frees the stream, after which no batch arrives */
void stop_history_stream(HistoryStream *stream);
