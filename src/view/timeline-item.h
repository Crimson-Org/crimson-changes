#pragma once

#include <glib-object.h>

#include "git/log.h"
#include "git/refs.h"
#include "graph/lanes.h"

typedef enum {
	/* The changes not committed yet, always the first row */
	TIMELINE_NOW,
	TIMELINE_STASH,
	/* A day's heading above its commits */
	TIMELINE_HEADING,
	TIMELINE_COMMIT,
} TimelineKind;

/* One row of the timeline */
#define TYPE_TIMELINE_ITEM (timeline_item_get_type())
G_DECLARE_FINAL_TYPE(TimelineItem, timeline_item, CHANGES, TIMELINE_ITEM, GObject)

struct _TimelineItem {
	GObject parent;
	TimelineKind kind;
	/* Set for commits and stashes, which are commits too */
	Commit *commit;
	char *stash_name;
	/* The heading's text */
	char *heading;
	/* How the history graph runs through this row, or NULL without a graph */
	GraphRow *graph;
};

/* Each takes what it is given */
TimelineItem *create_now_item(void);
TimelineItem *create_stash_item(const Stash *stash);
TimelineItem *create_heading_item(char *heading, GraphRow *graph);
TimelineItem *create_commit_item(Commit *commit, GraphRow *graph);
