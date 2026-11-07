#pragma once

#include <gtk/gtk.h>

#include "git/repository.h"
#include "view/timeline-item.h"

/* The widget of one timeline row, which shows any kind of item, so the list can reuse rows freely */
GtkWidget *create_timeline_row(void);
/* Shows the item, with the graph room for the given lanes, or no graph at all for a search result */
void bind_timeline_row(GtkWidget *row, TimelineItem *item, Repository *repository, int graph_columns,
	gboolean shows_graph);
/* The row's graph, which the timeline widens when wider history arrives */
GtkWidget *get_timeline_row_graph(GtkWidget *row);
/* Whether the widget sits in a row that can be chosen, rather than in a day's heading or outside every row */
gboolean is_in_choosable_row(GtkWidget *widget);
