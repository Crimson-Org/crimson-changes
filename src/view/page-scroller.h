#pragma once

#include <gtk/gtk.h>

/* A page's column, no wider than the content width, in a scroller that only scrolls down and has a button to go back
 * to the top. The returned widget is what the page shows */
GtkWidget *create_page_scroller(GtkWidget *content);
/* Puts a button to go back to the top over any scroller, showing once the view is a screen down. The returned widget
 * holds the scroller and is what the page shows */
GtkWidget *add_top_button(GtkScrolledWindow *scroller);
/* Glides a page made by either function above back to its top */
void glide_page_to_top(GtkWidget *page);
