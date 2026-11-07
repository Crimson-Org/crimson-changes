#pragma once

#include <gtk/gtk.h>

#include "git/log.h"
#include "window/window.h"

/* The menu of things to do with the commit shown, such as making a branch there or reverting it */
GtkWidget *create_commit_menu_button(Window *window);
/* A stash name makes it the menu of a stash, with Apply and Delete instead */
void set_commit_menu_commit(GtkWidget *button, const Commit *commit, const char *stash_name);
