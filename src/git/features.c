#include <stdlib.h>
#include <string.h>

#include "git/features.h"
#include "git/runner.h"

#define VERSION_MARK "git version "
#define MAJOR_VERSION 2
#define REWORD_MINOR 54
#define DROP_MINOR 56
#define DELETE_MERGED_MINOR 56
#define ADD_RESOLVED_MINOR 56
#define STRUCTURE_MINOR 52

static GitFeatures features;
static gboolean has_read_features;


void read_git_version(const char *text, int *major, int *minor) {
	const char *version = strstr(text, VERSION_MARK);
	*major = *minor = 0;
	if (!version)
		return;
	char *end = NULL;
	*major = (int)strtol(version + strlen(VERSION_MARK), &end, 10);
	if (end && *end == '.')
		*minor = (int)strtol(end + 1, NULL, 10);
}


static gboolean is_at_least(int minor) {
	return features.major > MAJOR_VERSION || (features.major == MAJOR_VERSION && features.minor >= minor);
}


const GitFeatures *get_git_features(void) {
	if (has_read_features)
		return &features;
	has_read_features = TRUE;
	const char *arguments[] = {"--version", NULL};
	GitResult *result = run_git_now(g_get_home_dir(), arguments, NULL);
	g_autofree char *text = get_git_output_text(result);
	free_git_result(result);
	read_git_version(text, &features.major, &features.minor);
	features.can_reword = is_at_least(REWORD_MINOR);
	features.can_drop = is_at_least(DROP_MINOR);
	features.can_delete_merged = is_at_least(DELETE_MERGED_MINOR);
	features.can_add_resolved = is_at_least(ADD_RESOLVED_MINOR);
	features.can_describe_structure = is_at_least(STRUCTURE_MINOR);
	g_message("Git %d.%d", features.major, features.minor);
	return &features;
}
