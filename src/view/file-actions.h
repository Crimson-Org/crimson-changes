#pragma once

#include "git/diff.h"
#include "window/window.h"

/* Where a file card's diff comes from, which decides what can be done with it */
typedef enum {
	/* In the work folder, not staged */
	SIDE_UNSTAGED,
	/* Staged for the next commit */
	SIDE_STAGED,
	/* Part of a commit */
	SIDE_COMMIT,
} DiffSide;

void stage_file(Window *window, const DiffFile *file);
void unstage_file(Window *window, const DiffFile *file);
/* Puts the file back as it was staged or committed, with Undo in the toast */
void discard_file(Window *window, const DiffFile *file);
void stage_hunk(Window *window, const DiffFile *file, const DiffHunk *hunk);
void unstage_hunk(Window *window, const DiffFile *file, const DiffHunk *hunk);
void discard_hunk(Window *window, const DiffFile *file, const DiffHunk *hunk);
void stage_all(Window *window);
void unstage_all(Window *window);
void discard_all(Window *window);
/* Puts every change aside in a stash under a name the user gives, which leaves the folder as last committed */
void stash_all(Window *window);
/* Writes the file as it was in the commit into the work folder, with Undo in the toast */
void restore_file_version(Window *window, const char *commit, const DiffFile *file);
/* For a file git stopped on in a merge: keep this side's version, the other side's, or mark it resolved by hand */
void keep_our_version(Window *window, const char *path);
void keep_their_version(Window *window, const char *path);
void mark_resolved(Window *window, const char *path);

void open_work_file(Window *window, const char *path);
void open_work_file_folder(Window *window, const char *path);
void copy_text(Window *window, const char *text, const char *toast);
