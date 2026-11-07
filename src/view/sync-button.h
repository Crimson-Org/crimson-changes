#pragma once

#include <gtk/gtk.h>

#include "window/window.h"

/* Fetch, pull and push in one menu, shown only for a repository with a remote, with the commits to get or send */
GtkWidget *create_sync_button(Window *window);
void set_sync_button_repository(GtkWidget *button, Repository *repository);
/* Looks for new commits on the remote quietly, such as when the window comes back into focus, at most every few
 * minutes */
void fetch_quietly(GtkWidget *button);
