#include "view/colours.h"

#define COLOUR_PRIORITY (GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 1)


void load_colour_variables(void) {
	g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(provider, ":root { --added: " ADDED_COLOUR "; --removed: " REMOVED_COLOUR
		"; --crimson: " CRIMSON_COLOUR "; --merge: " MERGE_COLOUR "; }");
	gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
		COLOUR_PRIORITY);
}


GdkRGBA get_colour_with_alpha(const char *colour, double alpha) {
	GdkRGBA rgba;
	gdk_rgba_parse(&rgba, colour);
	rgba.alpha = (float)alpha;
	return rgba;
}
