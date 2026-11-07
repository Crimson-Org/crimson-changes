#pragma once

#include <gtk/gtk.h>

#include "graph/lanes.h"

/* Draws one row of the history graph: smooth lane lines in their colours and the commit's dot */
GtkWidget *create_graph_view(void);
/* The row stays owned by the caller and must outlive the view's next draw */
void set_graph_view_row(GtkWidget *view, const GraphRow *row, gboolean is_current, gboolean is_merge);
/* How many lanes the view makes room for, beyond which lanes squeeze closer together */
void set_graph_view_columns(GtkWidget *view, int columns);
/* Where the first lane's centre is, so icons in rows without a graph line up with the dots */
int get_first_lane_centre(void);
/* The width the given lanes need */
int measure_graph_width(int columns);
