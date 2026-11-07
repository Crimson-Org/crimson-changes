#pragma once

#include <glib.h>

/* What changed on disk, as flags, so a burst of changes becomes one call */
typedef enum {
	/* A file in the work folder */
	WATCH_WORK_FOLDER = 1 << 0,
	/* The staging area */
	WATCH_INDEX = 1 << 1,
	/* A branch, a tag, the stash or the commit checked out */
	WATCH_REFERENCES = 1 << 2,
} WatchChanges;

typedef void (*FoldersChanged)(WatchChanges changes, gpointer data);

typedef struct FolderWatch FolderWatch;

/* Watches the work folder and the repository's own folders through the system's file events, so nothing polls.
 * The common folder holds the branches, which a linked work tree shares with its main repository */
FolderWatch *watch_repository_folders(const char *work_folder, const char *git_folder, const char *common_folder,
	FoldersChanged changed, gpointer data);
void stop_watching_folders(FolderWatch *watch);
