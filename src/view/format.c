#include <string.h>

#include "lang/en.h"
#include "view/format.h"

#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY 86400
#define DAYS_PER_WEEK 7
#define DAYS_PER_MONTH 30
#define DAYS_PER_YEAR 365
/* More numbers than days in any year, so the last day of a year never meets the first of the next */
#define DAY_NUMBERS_PER_YEAR 1000
/* Under this many seconds a change reads as just now */
#define JUST_NOW_SECONDS 45
#define PLAIN_TEXT_ICON "text-x-generic-symbolic"


static GDateTime *to_local(gint64 unix_time) {
	return g_date_time_new_from_unix_local(unix_time);
}


int get_day_number(gint64 unix_time) {
	g_autoptr(GDateTime) time = to_local(unix_time);
	return g_date_time_get_year(time) * DAY_NUMBERS_PER_YEAR + g_date_time_get_day_of_year(time);
}


/* Whole days between the day of the time and today, counted on the calendar */
static int count_days_ago(gint64 unix_time) {
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autoptr(GDateTime) then = to_local(unix_time);
	g_autoptr(GDateTime) today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
		g_date_time_get_day_of_month(now), 0, 0, 0);
	g_autoptr(GDateTime) that_day = g_date_time_new_local(g_date_time_get_year(then), g_date_time_get_month(then),
		g_date_time_get_day_of_month(then), 0, 0, 0);
	return (int)(g_date_time_difference(today, that_day) / G_TIME_SPAN_DAY);
}


static const char *const WEEKDAYS[] = {TEXT_WEEKDAYS};
static const char *const MONTHS[] = {TEXT_MONTHS};


/* GDateTime numbers weekdays from 1 for Monday and months from 1 for January */
static const char *get_weekday(GDateTime *time) {
	return WEEKDAYS[g_date_time_get_day_of_week(time) - 1];
}


static const char *get_month(GDateTime *time) {
	return MONTHS[g_date_time_get_month(time) - 1];
}


static char *format_date(gint64 unix_time) {
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autoptr(GDateTime) then = to_local(unix_time);
	if (g_date_time_get_year(now) == g_date_time_get_year(then))
		return g_strdup_printf(TEXT_DATE_THIS_YEAR, get_month(then), g_date_time_get_day_of_month(then));
	return g_strdup_printf(TEXT_DATE_OTHER_YEAR, get_month(then), g_date_time_get_day_of_month(then),
		g_date_time_get_year(then));
}


char *format_time_ago(gint64 unix_time) {
	gint64 seconds = g_get_real_time() / G_USEC_PER_SEC - unix_time;
	if (seconds < JUST_NOW_SECONDS)
		return g_strdup(TEXT_JUST_NOW);
	if (seconds < SECONDS_PER_HOUR) {
		int minutes = (int)MAX(1, seconds / SECONDS_PER_MINUTE);
		return g_strdup_printf(minutes == 1 ? TEXT_MINUTE_AGO : TEXT_MINUTES_AGO, minutes);
	}
	int days = count_days_ago(unix_time);
	if (days == 0) {
		int hours = (int)(seconds / SECONDS_PER_HOUR);
		return g_strdup_printf(hours == 1 ? TEXT_HOUR_AGO : TEXT_HOURS_AGO, hours);
	}
	if (days == 1)
		return g_strdup(TEXT_YESTERDAY_LOWER);
	if (days < DAYS_PER_WEEK * 2)
		return g_strdup_printf(TEXT_DAYS_AGO, days);
	if (days < DAYS_PER_MONTH * 2)
		return g_strdup_printf(TEXT_WEEKS_AGO, days / DAYS_PER_WEEK);
	if (days < DAYS_PER_YEAR)
		return g_strdup_printf(TEXT_MONTHS_AGO, days / DAYS_PER_MONTH);
	int years = days / DAYS_PER_YEAR;
	return g_strdup_printf(years == 1 ? TEXT_YEAR_AGO : TEXT_YEARS_AGO, years);
}


char *format_time_of_day(gint64 unix_time) {
	g_autoptr(GDateTime) then = to_local(unix_time);
	return g_strdup_printf(TEXT_TIME_OF_DAY, g_date_time_get_hour(then), g_date_time_get_minute(then));
}


char *format_full_time(gint64 unix_time) {
	g_autoptr(GDateTime) then = to_local(unix_time);
	return g_strdup_printf(TEXT_FULL_TIME, get_weekday(then), get_month(then), g_date_time_get_day_of_month(then),
		g_date_time_get_year(then), g_date_time_get_hour(then), g_date_time_get_minute(then));
}


char *format_day_heading(gint64 unix_time) {
	int days = count_days_ago(unix_time);
	if (days == 0)
		return g_strdup(TEXT_TODAY);
	if (days == 1)
		return g_strdup(TEXT_YESTERDAY);
	if (days > 0 && days < DAYS_PER_WEEK) {
		g_autoptr(GDateTime) then = to_local(unix_time);
		return g_strdup(get_weekday(then));
	}
	return format_date(unix_time);
}


char *format_file_count(guint count) {
	return g_strdup_printf(count == 1 ? TEXT_ONE_FILE : TEXT_FILES, count);
}


char *format_size(guint64 bytes) {
	return g_format_size(bytes);
}


const char *get_operation_title(Operation operation) {
	const char *const titles[] = {NULL, TEXT_OPERATION_MERGE, TEXT_OPERATION_REBASE, TEXT_OPERATION_CHERRY_PICK,
		TEXT_OPERATION_REVERT, TEXT_OPERATION_BISECT, TEXT_OPERATION_APPLY};
	return titles[operation];
}


char *get_path_folder(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? g_strndup(path, (gsize)(slash - path + 1)) : g_strdup("");
}


const char *get_path_name(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}


GIcon *get_file_icon(const char *path) {
	g_autofree char *type = g_content_type_guess(path, NULL, 0, NULL);
	/* A name without a known ending, such as TODO, is most likely text in a repository */
	if (g_content_type_is_unknown(type))
		return g_themed_icon_new(PLAIN_TEXT_ICON);
	GIcon *icon = g_content_type_get_symbolic_icon(type);
	return icon ? icon : g_themed_icon_new(PLAIN_TEXT_ICON);
}
