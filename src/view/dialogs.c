#include "lang/en.h"
#include "view/dialogs.h"

#define CONFIRM_RESPONSE "confirm"
#define CANCEL_RESPONSE "cancel"
#define MESSAGE_HEIGHT 120
#define DIALOG_KEY "request"

typedef struct {
	TextChosen chosen;
	Confirmed confirmed;
	gpointer data;
	GDestroyNotify destroy;
	GtkWidget *entry, *text_view;
} DialogRequest;


static void free_dialog_request(gpointer data) {
	DialogRequest *request = data;
	if (request->destroy)
		request->destroy(request->data);
	g_free(request);
}


static char *read_typed_text(DialogRequest *request) {
	if (request->entry)
		return g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(request->entry))));
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(request->text_view));
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	return g_strstrip(gtk_text_buffer_get_text(buffer, &start, &end, FALSE));
}


static void respond(AdwAlertDialog *dialog, const char *response, DialogRequest *request) {
	if (g_strcmp0(response, CONFIRM_RESPONSE) != 0)
		return;
	if (request->confirmed)
		request->confirmed(request->data);
	else {
		g_autofree char *text = read_typed_text(request);
		request->chosen(text, request->data);
	}
}


static AdwDialog *create_dialog(const char *heading, const char *body, const char *confirm, gboolean is_destructive,
	DialogRequest *request) {
	AdwDialog *dialog = adw_alert_dialog_new(heading, body);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), CANCEL_RESPONSE, TEXT_CANCEL, CONFIRM_RESPONSE, confirm,
		NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), CONFIRM_RESPONSE,
		is_destructive ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), CONFIRM_RESPONSE);
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), CANCEL_RESPONSE);
	g_object_set_data_full(G_OBJECT(dialog), DIALOG_KEY, request, free_dialog_request);
	g_signal_connect(dialog, "response", G_CALLBACK(respond), request);
	return dialog;
}


static void follow_typing(GtkWidget *widget, AdwAlertDialog *dialog) {
	DialogRequest *request = g_object_get_data(G_OBJECT(dialog), DIALOG_KEY);
	g_autofree char *text = read_typed_text(request);
	adw_alert_dialog_set_response_enabled(dialog, CONFIRM_RESPONSE, *text != 0);
}


void ask_for_text(GtkWidget *parent, const char *heading, const char *body, const char *initial,
	const char *confirm, TextChosen chosen, gpointer data, GDestroyNotify destroy) {
	DialogRequest *request = g_new0(DialogRequest, 1);
	request->chosen = chosen;
	request->data = data;
	request->destroy = destroy;
	AdwDialog *dialog = create_dialog(heading, body, confirm, FALSE, request);
	request->entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(request->entry), initial ? initial : "");
	g_signal_connect(request->entry, "changed", G_CALLBACK(follow_typing), dialog);
	gtk_entry_set_activates_default(GTK_ENTRY(request->entry), TRUE);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), request->entry);
	follow_typing(request->entry, ADW_ALERT_DIALOG(dialog));
	adw_dialog_present(dialog, parent);
	gtk_widget_grab_focus(request->entry);
}


void ask_for_message(GtkWidget *parent, const char *heading, const char *body, const char *initial,
	const char *confirm, TextChosen chosen, gpointer data, GDestroyNotify destroy) {
	DialogRequest *request = g_new0(DialogRequest, 1);
	request->chosen = chosen;
	request->data = data;
	request->destroy = destroy;
	AdwDialog *dialog = create_dialog(heading, body, confirm, FALSE, request);
	request->text_view = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(request->text_view), GTK_WRAP_WORD_CHAR);
	gtk_widget_add_css_class(request->text_view, "commit-message");
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(request->text_view));
	gtk_text_buffer_set_text(buffer, initial ? initial : "", -1);
	g_signal_connect_swapped(buffer, "changed", G_CALLBACK(follow_typing), dialog);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), request->text_view);
	gtk_widget_set_size_request(scroller, -1, MESSAGE_HEIGHT);
	gtk_widget_add_css_class(scroller, "message-frame");
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroller);
	follow_typing(request->text_view, ADW_ALERT_DIALOG(dialog));
	adw_dialog_present(dialog, parent);
	gtk_widget_grab_focus(request->text_view);
}


void ask_to_confirm(GtkWidget *parent, const char *heading, const char *body, const char *confirm,
	gboolean is_destructive, Confirmed confirmed, gpointer data, GDestroyNotify destroy) {
	DialogRequest *request = g_new0(DialogRequest, 1);
	request->confirmed = confirmed;
	request->data = data;
	request->destroy = destroy;
	AdwDialog *dialog = create_dialog(heading, body, confirm, is_destructive, request);
	adw_dialog_present(dialog, parent);
}
