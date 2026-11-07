#pragma once

#include <gtk/gtk.h>

#include "git/diff.h"

/* One hunk as code in the file's colours, with added and removed lines tinted, the words that changed inside a line
 * marked stronger, and the old and new line numbers beside it, in a scroller that only scrolls sideways */
GtkWidget *create_hunk_view(const DiffFile *file, const DiffHunk *hunk);
/* Any text as code in the file's colours, with line numbers from first_line, such as a blamed file */
GtkWidget *create_code_view(const char *path, const char *text, int first_line);
/* The colours follow the light or dark style, and the size and wrapping follow the settings */
void apply_code_view_settings(GtkWidget *view);
