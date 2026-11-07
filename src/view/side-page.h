#pragma once

#include <adwaita.h>

/* A page that slides in over the main pane with its own header and a way back, such as a file's blame */
AdwNavigationPage *create_side_page(const char *title, const char *subtitle, GtkWidget *content,
	GtkWidget *end_widget);
/* The page's content area, which holds a spinner until the content is read */
void set_side_page_content(AdwNavigationPage *page, GtkWidget *content);
