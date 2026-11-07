#pragma once

#include <gtk/gtk.h>

#include "window/window.h"

/* The page before any folder is open: an Open button and the repositories opened before */
GtkWidget *create_start_page(Window *window);
void update_start_page(GtkWidget *page);

/* The page for a folder without a repository, which offers to start one */
GtkWidget *create_setup_page(Window *window);
void set_setup_page_folder(GtkWidget *page, const char *folder);

/* The page for a folder that cannot be shown, such as when git is missing */
GtkWidget *create_problem_page(Window *window);
void set_problem_page_text(GtkWidget *page, const char *title, const char *description);
