#pragma once

#include <gtk/gtk.h>

/* The colours of added and removed lines and of Crimson's own red, defined once here for the code and handed to the
 * stylesheet as --added, --removed and --crimson */
#define ADDED_COLOUR "#2ec27e"
#define REMOVED_COLOUR "#ed333b"
#define CRIMSON_COLOUR "#e5484d"
/* Merges share the purple of a lane in the graph */
#define MERGE_COLOUR "#8e4ec6"

/* Gives the stylesheet the colours above, once at start */
void load_colour_variables(void);
/* The colour with its alpha changed, for tinting text without CSS */
GdkRGBA get_colour_with_alpha(const char *colour, double alpha);
