#pragma once

#include <gtk/gtk.h>

#include "common/text/paths.h"

#include "git/repository.h"

/* How long ago, such as "just now", "5 minutes ago" or "3 days ago", and a date further back */
char *format_time_ago(gint64 unix_time);
/* The hour and minute, such as "14:05" */
char *format_time_of_day(gint64 unix_time);
/* A full date and time, for tooltips and details */
char *format_full_time(gint64 unix_time);
/* The heading over a day of commits, such as "Today", "Monday" or "March 3, 2024" */
char *format_day_heading(gint64 unix_time);
/* A number that identifies the day in local time, so commits can be grouped by day */
int get_day_number(gint64 unix_time);
/* Such as "3 files" or "1 file" */
char *format_file_count(guint count);
/* A size in bytes as people read it, such as "1.2 MB" */
char *format_size(guint64 bytes);
/* What git is in the middle of, such as "Merge in Progress", or NULL */
const char *get_operation_title(Operation operation);
/* A path's folder part, with the trailing slash, or an empty string */
char *get_path_folder(const char *path);
const char *get_path_name(const char *path);
/* The icon for a file kind guessed from its name, such as text-x-script for a shell script */
GIcon *get_file_icon(const char *path);
