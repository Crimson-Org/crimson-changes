#pragma once

#include <gtk/gtk.h>

#include "window/window.h"

/* The changes not committed yet: what is staged, what is only in the work folder, conflicts, and the commit box */
typedef struct ChangesPage ChangesPage;

ChangesPage *create_changes_page(Window *window);
GtkWidget *get_changes_page_widget(ChangesPage *page);
void glide_changes_page_to_top(ChangesPage *page);
void set_changes_page_repository(ChangesPage *page, Repository *repository);
void refresh_changes_page(ChangesPage *page);
void free_changes_page(ChangesPage *page);
