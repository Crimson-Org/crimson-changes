#include <string.h>

#include "graph/lanes.h"

/* A lane waits for one commit, the parent of the last commit drawn in it, or is free when hash is NULL */
typedef struct {
	char *hash;
	int colour;
} Lane;

struct LaneLayout {
	GArray *lanes;
	int next_colour;
};


static void clear_lane(gpointer data) {
	Lane *lane = data;
	g_clear_pointer(&lane->hash, g_free);
}


LaneLayout *create_lane_layout(void) {
	LaneLayout *layout = g_new0(LaneLayout, 1);
	layout->lanes = g_array_new(FALSE, TRUE, sizeof(Lane));
	g_array_set_clear_func(layout->lanes, clear_lane);
	return layout;
}


void free_lane_layout(LaneLayout *layout) {
	if (!layout)
		return;
	g_array_unref(layout->lanes);
	g_free(layout);
}


static Lane *get_lane(LaneLayout *layout, int column) {
	return &g_array_index(layout->lanes, Lane, column);
}


static int find_lane(LaneLayout *layout, const char *hash, int skipped) {
	for (guint column = 0; column < layout->lanes->len; column++) {
		Lane *lane = get_lane(layout, column);
		if ((int)column != skipped && lane->hash && strcmp(lane->hash, hash) == 0)
			return (int)column;
	}
	return -1;
}


/* The first free lane, or a new one at the right, never the skipped one */
static int open_lane(LaneLayout *layout, const char *hash, int skipped) {
	int column = -1;
	for (guint index = 0; index < layout->lanes->len && column < 0; index++)
		if ((int)index != skipped && !get_lane(layout, index)->hash)
			column = (int)index;
	if (column < 0) {
		column = (int)layout->lanes->len;
		g_array_set_size(layout->lanes, layout->lanes->len + 1);
	}
	Lane *lane = get_lane(layout, column);
	lane->hash = g_strdup(hash);
	lane->colour = layout->next_colour++;
	return column;
}


static void add_line(GraphRow *row, GArray *lines, int from, int to, int colour) {
	LaneLine line = {from, to, colour};
	g_array_append_val(lines, line);
	row->width = MAX(row->width, MAX(from, to) + 1);
}


static GraphRow *create_row(void) {
	GraphRow *row = g_new0(GraphRow, 1);
	row->upper = g_array_new(FALSE, FALSE, sizeof(LaneLine));
	row->lower = g_array_new(FALSE, FALSE, sizeof(LaneLine));
	return row;
}


/* Free lanes at the right end take no room */
static void trim_lanes(LaneLayout *layout) {
	while (layout->lanes->len > 0 && !get_lane(layout, layout->lanes->len - 1)->hash)
		g_array_set_size(layout->lanes, layout->lanes->len - 1);
}


/* Every lane that waited for this commit runs into its dot, and all but the dot's own lane end there */
static void join_children(LaneLayout *layout, GraphRow *row, const char *hash) {
	for (guint column = 0; column < layout->lanes->len; column++) {
		Lane *lane = get_lane(layout, column);
		if (!lane->hash)
			continue;
		gboolean is_waiting = strcmp(lane->hash, hash) == 0;
		add_line(row, row->upper, (int)column, is_waiting ? row->node_column : (int)column, lane->colour);
		if (is_waiting && (int)column != row->node_column)
			g_clear_pointer(&lane->hash, g_free);
	}
}


/* The dot's lane goes on to the first parent, and every other parent gets the lane already waiting for it or a new
 * one, which starts at the dot */
static GArray *branch_to_parents(LaneLayout *layout, GraphRow *row, const char *const *parents) {
	GArray *started = g_array_new(FALSE, FALSE, sizeof(int));
	Lane *own = get_lane(layout, row->node_column);
	if (!parents || !parents[0]) {
		g_clear_pointer(&own->hash, g_free);
		return started;
	}
	g_set_str(&own->hash, parents[0]);
	for (const char *const *parent = parents + 1; *parent; parent++) {
		int column = find_lane(layout, *parent, row->node_column);
		if (column < 0) {
			column = open_lane(layout, *parent, row->node_column);
			g_array_append_val(started, column);
		}
		add_line(row, row->lower, row->node_column, column, get_lane(layout, column)->colour);
	}
	return started;
}


static gboolean was_started(GArray *started, int column) {
	for (guint index = 0; index < started->len; index++)
		if (g_array_index(started, int, index) == column)
			return TRUE;
	return FALSE;
}


GraphRow *add_graph_row(LaneLayout *layout, const char *hash, const char *const *parents) {
	GraphRow *row = create_row();
	row->node_column = find_lane(layout, hash, -1);
	gboolean has_child = row->node_column >= 0;
	if (!has_child)
		row->node_column = open_lane(layout, hash, -1);
	row->node_colour = get_lane(layout, row->node_column)->colour;
	row->width = row->node_column + 1;
	if (has_child)
		join_children(layout, row, hash);
	else
		for (guint column = 0; column < layout->lanes->len; column++) {
			Lane *lane = get_lane(layout, column);
			if (lane->hash && (int)column != row->node_column)
				add_line(row, row->upper, (int)column, (int)column, lane->colour);
		}
	g_autoptr(GArray) started = branch_to_parents(layout, row, parents);
	for (guint column = 0; column < layout->lanes->len; column++) {
		Lane *lane = get_lane(layout, column);
		if (lane->hash && !was_started(started, (int)column))
			add_line(row, row->lower, (int)column, (int)column, lane->colour);
	}
	trim_lanes(layout);
	return row;
}


GraphRow *create_passing_row(LaneLayout *layout) {
	GraphRow *row = create_row();
	row->node_column = -1;
	for (guint column = 0; column < layout->lanes->len; column++) {
		Lane *lane = get_lane(layout, column);
		if (!lane->hash)
			continue;
		add_line(row, row->upper, (int)column, (int)column, lane->colour);
		add_line(row, row->lower, (int)column, (int)column, lane->colour);
	}
	return row;
}


void free_graph_row(GraphRow *row) {
	if (!row)
		return;
	g_array_unref(row->upper);
	g_array_unref(row->lower);
	g_free(row);
}
