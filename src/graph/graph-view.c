#include <math.h>

#include "graph/graph-view.h"
#include "view/colours.h"

#define LANE_WIDTH 16
#define SIDE_PADDING 10
#define LINE_WIDTH 2.0
#define DOT_RADIUS 4.5
/* The checked out commit gets a ring around its dot, so it stands out in a long history */
#define CURRENT_RING_RADIUS 7.5
#define CURRENT_RING_WIDTH 1.5
#define MERGE_DOT_RADIUS 3.5
#define ROW_KEY "graph-row"
#define CURRENT_KEY "is-current"
#define MERGE_KEY "is-merge"
#define COLUMNS_KEY "columns"
/* Lanes past this many squeeze together instead of pushing the messages away */
#define MOST_ROOMY_COLUMNS 6
#define FULL_TURN (2 * G_PI)
#define HALF 0.5

/* Lane colours that read on both light and dark backgrounds, starting with Crimson's own red */
static const char *const LANE_COLOURS[] = {
	CRIMSON_COLOUR, "#3e9bff", "#46a758", "#f5a524", MERGE_COLOUR, "#12a594", "#e93d82", "#ff8b3e",
};


static const GdkRGBA *get_lane_colour(int colour) {
	static GdkRGBA parsed[G_N_ELEMENTS(LANE_COLOURS)];
	static gboolean has_parsed;
	if (!has_parsed) {
		for (guint index = 0; index < G_N_ELEMENTS(LANE_COLOURS); index++)
			gdk_rgba_parse(&parsed[index], LANE_COLOURS[index]);
		has_parsed = TRUE;
	}
	return &parsed[colour % (int)G_N_ELEMENTS(LANE_COLOURS)];
}


int get_first_lane_centre(void) {
	return SIDE_PADDING + LANE_WIDTH / 2;
}


int measure_graph_width(int columns) {
	return SIDE_PADDING * 2 + MIN(MAX(columns, 1), MOST_ROOMY_COLUMNS) * LANE_WIDTH;
}


/* A lane's centre, with lanes squeezed together when the row has more than fit */
static double get_lane_x(int column, int columns, int width) {
	double spacing = MIN((double)LANE_WIDTH, (double)(width - SIDE_PADDING * 2) / MAX(columns, 1));
	return SIDE_PADDING + spacing * (column + HALF);
}


/* A line from one column to another that leaves and arrives straight up and down, so lanes flow into each other */
static void draw_line(cairo_t *cairo, const LaneLine *line, int columns, int width, double top, double bottom) {
	double from = get_lane_x(line->from, columns, width), to = get_lane_x(line->to, columns, width);
	gdk_cairo_set_source_rgba(cairo, get_lane_colour(line->colour));
	cairo_move_to(cairo, from, top);
	if (line->from == line->to)
		cairo_line_to(cairo, to, bottom);
	else {
		double middle = (top + bottom) * HALF;
		cairo_curve_to(cairo, from, middle, to, middle, to, bottom);
	}
	cairo_stroke(cairo);
}


static void draw_lines(cairo_t *cairo, GArray *lines, int columns, int width, double top, double bottom) {
	for (guint index = 0; index < lines->len; index++)
		draw_line(cairo, &g_array_index(lines, LaneLine, index), columns, width, top, bottom);
}


static void draw_dot(cairo_t *cairo, GtkWidget *view, const GraphRow *row, int columns, int width, int height) {
	double x = get_lane_x(row->node_column, columns, width), y = height * HALF;
	const GdkRGBA *colour = get_lane_colour(row->node_colour);
	gboolean is_merge = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), MERGE_KEY));
	if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), CURRENT_KEY))) {
		cairo_set_line_width(cairo, CURRENT_RING_WIDTH);
		gdk_cairo_set_source_rgba(cairo, colour);
		cairo_arc(cairo, x, y, CURRENT_RING_RADIUS, 0, FULL_TURN);
		cairo_stroke(cairo);
	}
	cairo_arc(cairo, x, y, is_merge ? MERGE_DOT_RADIUS : DOT_RADIUS, 0, FULL_TURN);
	gdk_cairo_set_source_rgba(cairo, colour);
	cairo_fill(cairo);
}


static void draw_graph(GtkDrawingArea *area, cairo_t *cairo, int width, int height, gpointer data) {
	const GraphRow *row = g_object_get_data(G_OBJECT(area), ROW_KEY);
	if (!row)
		return;
	int columns = MAX(row->width, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), COLUMNS_KEY)));
	cairo_set_line_width(cairo, LINE_WIDTH);
	cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
	double middle = height * HALF;
	draw_lines(cairo, row->upper, columns, width, 0, middle);
	draw_lines(cairo, row->lower, columns, width, middle, height);
	if (row->node_column >= 0)
		draw_dot(cairo, GTK_WIDGET(area), row, columns, width, height);
}


GtkWidget *create_graph_view(void) {
	GtkWidget *view = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(view), draw_graph, NULL, NULL);
	gtk_widget_set_vexpand(view, TRUE);
	set_graph_view_columns(view, 1);
	return view;
}


void set_graph_view_row(GtkWidget *view, const GraphRow *row, gboolean is_current, gboolean is_merge) {
	g_object_set_data(G_OBJECT(view), ROW_KEY, (gpointer)row);
	g_object_set_data(G_OBJECT(view), CURRENT_KEY, GINT_TO_POINTER(is_current));
	g_object_set_data(G_OBJECT(view), MERGE_KEY, GINT_TO_POINTER(is_merge));
	gtk_widget_queue_draw(view);
}


void set_graph_view_columns(GtkWidget *view, int columns) {
	g_object_set_data(G_OBJECT(view), COLUMNS_KEY, GINT_TO_POINTER(columns));
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(view), measure_graph_width(columns));
	gtk_widget_queue_draw(view);
}
