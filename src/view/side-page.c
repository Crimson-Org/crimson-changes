#include "common/widgets/loading.h"
#include "view/side-page.h"

#define STACK_KEY "content-stack"
#define LOADING_PAGE "loading"
#define CONTENT_PAGE "content"


AdwNavigationPage *create_side_page(const char *title, const char *subtitle, GtkWidget *content,
	GtkWidget *end_widget) {
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "title-bar");
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new(title, subtitle ? subtitle : ""));
	if (end_widget)
		adw_header_bar_pack_end(ADW_HEADER_BAR(header), end_widget);
	GtkWidget *stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	GtkWidget *spinner = create_loading_spinner();
	gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
	gtk_stack_add_named(GTK_STACK(stack), spinner, LOADING_PAGE);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);
	AdwNavigationPage *page = adw_navigation_page_new(toolbar, title);
	g_object_set_data(G_OBJECT(page), STACK_KEY, stack);
	if (content)
		set_side_page_content(page, content);
	return page;
}


void set_side_page_content(AdwNavigationPage *page, GtkWidget *content) {
	GtkWidget *stack = g_object_get_data(G_OBJECT(page), STACK_KEY);
	GtkWidget *old = gtk_stack_get_child_by_name(GTK_STACK(stack), CONTENT_PAGE);
	if (old)
		gtk_stack_remove(GTK_STACK(stack), old);
	gtk_stack_add_named(GTK_STACK(stack), content, CONTENT_PAGE);
	gtk_stack_set_visible_child_name(GTK_STACK(stack), CONTENT_PAGE);
}
