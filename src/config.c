/*
 * INI-style config file parser for machismo.
 * Minimal implementation — no external dependencies.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char* strip(char* s)
{
	while (*s && isspace((unsigned char)*s)) s++;
	char* end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) end--;
	*end = '\0';
	return s;
}

int config_load(const char* path, machismo_config_t* cfg)
{
	FILE* f = fopen(path, "r");
	if (!f) return -1;

	memset(cfg, 0, sizeof(*cfg));

	char line[512];
	machismo_trampoline_config_t* cur_tramp = NULL;

	while (fgets(line, sizeof(line), f)) {
		char* s = strip(line);

		/* Skip empty lines and comments */
		if (!*s || *s == '#' || *s == ';') continue;

		/* Section header */
		if (*s == '[') {
			char* end = strchr(s, ']');
			if (!end) continue;
			*end = '\0';
			s++;

			cur_tramp = NULL;

			if (strcmp(s, "general") == 0) {
				/* general section — handled by key=value below */
			} else if (strncmp(s, "trampoline.", 11) == 0) {
				const char* name = s + 11;
				if (cfg->num_trampolines < CONFIG_MAX_TRAMPOLINES) {
					cur_tramp = &cfg->trampolines[cfg->num_trampolines++];
					memset(cur_tramp, 0, sizeof(*cur_tramp));
					cur_tramp->name = strdup(name);
				}
			}
			continue;
		}

		/* Key = value */
		char* eq = strchr(s, '=');
		if (!eq) continue;
		*eq = '\0';
		char* key = strip(s);
		char* val = strip(eq + 1);

		if (cur_tramp) {
			/* Inside a [trampoline.*] section */
			if (strcmp(key, "lib") == 0) {
				free(cur_tramp->lib);
				cur_tramp->lib = strdup(val);
			} else if (strcmp(key, "prefix") == 0) {
				if (cur_tramp->num_prefixes < CONFIG_MAX_PREFIXES)
					cur_tramp->prefixes[cur_tramp->num_prefixes++] = strdup(val);
			} else if (strcmp(key, "init_wrapper") == 0) {
				cur_tramp->init_wrapper = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
			} else if (strcmp(key, "renderer") == 0) {
				free(cur_tramp->renderer);
				cur_tramp->renderer = strdup(val);
			}
		} else {
			/* [general] section or before any section */
			if (strcmp(key, "dylib_map") == 0) {
				free(cfg->dylib_map);
				cfg->dylib_map = strdup(val);
			} else if (strcmp(key, "patches") == 0) {
				free(cfg->patches);
				cfg->patches = strdup(val);
			}
		}
	}

	fclose(f);
	return 0;
}

void config_free(machismo_config_t* cfg)
{
	free(cfg->dylib_map);
	free(cfg->patches);
	for (int i = 0; i < cfg->num_trampolines; i++) {
		free(cfg->trampolines[i].name);
		free(cfg->trampolines[i].lib);
		free(cfg->trampolines[i].renderer);
		for (int j = 0; j < cfg->trampolines[i].num_prefixes; j++)
			free(cfg->trampolines[i].prefixes[j]);
	}
	memset(cfg, 0, sizeof(*cfg));
}
