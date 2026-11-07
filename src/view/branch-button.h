#pragma once

#include <gtk/gtk.h>

#include "window/window.h"

/* The repository's name with the branch and how far it is ahead or behind, which opens the branch list: switch,
 * create, rename, merge, delete and clean up branches, with remote branches and tags one fold away */
GtkWidget *create_branch_button(Window *window);
void set_branch_button_repository(GtkWidget *button, Repository *repository);
void open_branch_list(GtkWidget *button);
