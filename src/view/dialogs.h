#pragma once

#include <adwaita.h>

/* Called with the text typed, after the user confirmed */
typedef void (*TextChosen)(const char *text, gpointer data);
typedef void (*Confirmed)(gpointer data);

/* Asks for one line, such as a branch name, with the confirm button only active once something is typed */
void ask_for_text(GtkWidget *parent, const char *heading, const char *body, const char *initial,
	const char *confirm, TextChosen chosen, gpointer data, GDestroyNotify destroy);
/* Asks for several lines, such as a commit message */
void ask_for_message(GtkWidget *parent, const char *heading, const char *body, const char *initial,
	const char *confirm, TextChosen chosen, gpointer data, GDestroyNotify destroy);
/* Asks before something that cannot be taken back, with the confirm button in red when it destroys work */
void ask_to_confirm(GtkWidget *parent, const char *heading, const char *body, const char *confirm,
	gboolean is_destructive, Confirmed confirmed, gpointer data, GDestroyNotify destroy);
