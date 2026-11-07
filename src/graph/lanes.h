#pragma once

#include <glib.h>

/* A line through one half of a row, from a column at one edge to a column at the other, in its lane's colour */
typedef struct {
	int from, to;
	int colour;
} LaneLine;

/* How one commit's row of the graph is drawn: the commit's dot, the lines from the row's top to the dot's height,
 * and the lines from there to the row's bottom */
typedef struct {
	int node_column, node_colour;
	GArray *upper, *lower;
	/* The columns this row uses */
	int width;
} GraphRow;

/* The lanes open between rows, carried from one commit to the next, so history can be laid out a page at a time */
typedef struct LaneLayout LaneLayout;

LaneLayout *create_lane_layout(void);
void free_lane_layout(LaneLayout *layout);
/* Lays out the next commit, which must come after all its children, as git log's date or topology order gives */
GraphRow *add_graph_row(LaneLayout *layout, const char *hash, const char *const *parents);
/* Straight lines for every lane still open after the last commit, for rows between commits such as headings */
GraphRow *create_passing_row(LaneLayout *layout);
void free_graph_row(GraphRow *row);
