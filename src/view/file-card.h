#pragma once

#include <gtk/gtk.h>

#include "git/diff.h"
#include "view/file-actions.h"
#include "window/window.h"

/* Where the two versions of a file card's diff come from, for pictures and Restore */
typedef struct {
	DiffSide side;
	/* The commit shown, and its first parent, for a commit's cards */
	const char *commit, *parent;
} CardSource;

/* One changed file: a header with its name, kind and line counts and its actions, over its diff, which folds away.
 * The card keeps the list holding the file alive, so a newer read of the folder never frees it under the card */
GtkWidget *create_file_card(Window *window, const CardSource *source, GPtrArray *files, const DiffFile *file,
	gboolean is_expanded);
/* A file git stopped on in a merge, with ways to settle it */
GtkWidget *create_conflict_card(Window *window, const char *path);
/* A one line summary of a file for the list of changed files: its icon, path, kind and line counts */
GtkWidget *create_file_overview_row(const DiffFile *file);
/* The fingerprint of what the card shows, so a list only rebuilds cards whose diff changed */
guint get_file_card_hash(GtkWidget *card);
void set_file_card_expanded(GtkWidget *card, gboolean is_expanded);
/* Size and wrapping changed, so the code views are made again */
void refresh_file_card(GtkWidget *card);
