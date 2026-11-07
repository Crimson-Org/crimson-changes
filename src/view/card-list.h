#pragma once

#include <gtk/gtk.h>

#include "view/file-card.h"

/* A column of file cards that keeps the cards whose diff did not change, so a fresh read of the folder never moves
 * the view, while new cards slide in and gone ones slide out */
typedef struct CardList CardList;

/* The list of changed files shows once there are at least this many files */
CardList *create_card_list(guint fewest_overview_files);
/* The cards */
GtkWidget *get_card_list_widget(CardList *list);
/* The short list of the changed files, which jumps to a file's card, placed wherever the page wants it */
GtkWidget *get_card_list_overview(CardList *list);
/* Shows the files in shown, which owner holds and the cards keep alive. Without animation everything is replaced at
 * once, such as when another commit is shown */
void update_card_list(CardList *list, Window *window, const CardSource *source, GPtrArray *owner, GPtrArray *shown,
	gboolean animates);
/* Makes every code view again, such as after the text size changed */
void refresh_card_list(CardList *list);
void free_card_list(CardList *list);
