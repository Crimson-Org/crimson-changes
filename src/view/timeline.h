#pragma once

#include <gtk/gtk.h>

#include "git/repository.h"
#include "view/timeline-item.h"

/* The list down the side: the uncommitted changes, the stashes and the history as a graph grouped by day */
typedef struct Timeline Timeline;

/* Called when a row is chosen, with the item the row shows, and whether the user chose it rather than the timeline
 * keeping its choice through a reload */
typedef void (*TimelineChosen)(TimelineItem *item, gboolean is_users_choice, gpointer data);
/* Called when the user clicks a row, the chosen one too, after any change of choice */
typedef void (*TimelinePressed)(gpointer data);
/* Called when a commit asked for with select_timeline_commit cannot be found in the history shown */
typedef void (*CommitMissing)(const char *hash, gpointer data);

Timeline *create_timeline(TimelineChosen chosen, TimelinePressed pressed, CommitMissing missing, gpointer data);
GtkWidget *get_timeline_widget(Timeline *timeline);
/* The bar with the search field, which goes under the side panel's header */
GtkWidget *get_timeline_search_bar(Timeline *timeline);
void set_timeline_repository(Timeline *timeline, Repository *repository);
/* Reads the history again, such as when every branch should show */
void reload_timeline_history(Timeline *timeline);
/* Selects the commit, reading further back until it is found */
void select_timeline_commit(Timeline *timeline, const char *hash);
void select_timeline_now(Timeline *timeline);
void free_timeline(Timeline *timeline);
