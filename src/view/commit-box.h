#pragma once

#include <gtk/gtk.h>

#include "window/window.h"

/* The message field and Commit button under the uncommitted changes */
typedef struct CommitBox CommitBox;

CommitBox *create_commit_box(Window *window);
GtkWidget *get_commit_box_widget(CommitBox *box);
/* The button names what it commits: the staged files, or everything when nothing is staged */
void update_commit_box(CommitBox *box, guint staged_count, guint changed_count);
void reset_commit_box(CommitBox *box);
void free_commit_box(CommitBox *box);
