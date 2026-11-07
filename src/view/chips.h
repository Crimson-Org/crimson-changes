#pragma once

#include <gtk/gtk.h>

/* Small coloured labels for the branches and tags that point at a commit, in one look everywhere */
void fill_reference_chips(GtkWidget *box, GPtrArray *references);
void clear_reference_chips(GtkWidget *box);
/* A text followed by the added and removed line counts, each in a colour that follows the light or dark style.
 * The text ends in "..." when it does not fit, and a count of 0 hides */
GtkWidget *create_change_counts(void);
void set_change_counts(GtkWidget *counts, const char *text, int added, int removed);
/* One chip, such as for the branch a stash came from */
GtkWidget *create_chip(const char *text, const char *kind_class);
