#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void eb_config_defaults(eb_config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->device, sizeof(c->device), "auto");
	c->left = EB_CH_BRIGHTNESS;
	c->right = EB_CH_VOLUME;
	c->edge_width = 0.06;
	c->active_area = 1.0;
	c->travel = 0.8;
	c->step = 2;
	c->quick_mute = true;
	c->media_strip = true;
	c->media_reverse = false;
	c->media_double_tap = false;
	c->bottom_height = 0.08;
	c->osd = true;
	c->claim_mm = 1.5;
	c->release_mm = 3.0;
	c->swipe_mm = 12.0;
	c->tap_ms = 250;
	c->double_tap_ms = 350;
	c->debug = false;
}

const char *eb_channel_name(int ch)
{
	switch (ch) {
	case EB_CH_VOLUME: return "volume";
	case EB_CH_BRIGHTNESS: return "brightness";
	default: return "none";
	}
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static int parse_bool(const char *v, bool *out)
{
	if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) {
		*out = true;
		return 0;
	}
	if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) {
		*out = false;
		return 0;
	}
	return -1;
}

static int parse_double(const char *v, double lo, double hi, double *out)
{
	char *end;
	errno = 0;
	double d = strtod(v, &end);
	if (errno || end == v || *trim(end) || d < lo || d > hi)
		return -1;
	*out = d;
	return 0;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
	char *end;
	errno = 0;
	long n = strtol(v, &end, 10);
	if (errno || end == v || *trim(end) || n < lo || n > hi)
		return -1;
	*out = (int)n;
	return 0;
}

static int parse_channel(const char *v, int *out)
{
	if (!strcasecmp(v, "volume")) *out = EB_CH_VOLUME;
	else if (!strcasecmp(v, "brightness")) *out = EB_CH_BRIGHTNESS;
	else if (!strcasecmp(v, "none") || !strcasecmp(v, "off")) *out = EB_CH_NONE;
	else return -1;
	return 0;
}

int eb_config_load(eb_config *c, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return errno == ENOENT ? 0 : -1;

	eb_config n = *c;
	char line[512];
	int lineno = 0, rc = 0;

	while (fgets(line, sizeof(line), f)) {
		lineno++;
		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		char *s = trim(line);
		if (!*s)
			continue;

		char *eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "omarchy-edges: %s:%d: expected key = value\n", path, lineno);
			rc = -1;
			continue;
		}
		*eq = '\0';
		char *k = trim(s), *v = trim(eq + 1);
		/* Allow quoted values. */
		size_t vl = strlen(v);
		if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
			v[vl - 1] = '\0';
			v++;
		}

		int bad = 0;
		if (!strcmp(k, "device")) {
			if (strlen(v) >= sizeof(n.device)) bad = 1;
			else snprintf(n.device, sizeof(n.device), "%s", v);
		}
		else if (!strcmp(k, "left")) bad = parse_channel(v, &n.left);
		else if (!strcmp(k, "right")) bad = parse_channel(v, &n.right);
		else if (!strcmp(k, "edge_width")) bad = parse_double(v, 0.01, 0.4, &n.edge_width);
		else if (!strcmp(k, "active_area")) bad = parse_double(v, 0.1, 1.0, &n.active_area);
		else if (!strcmp(k, "travel")) bad = parse_double(v, 0.1, 3.0, &n.travel);
		else if (!strcmp(k, "step")) bad = parse_int(v, 1, 25, &n.step);
		else if (!strcmp(k, "quick_mute")) bad = parse_bool(v, &n.quick_mute);
		else if (!strcmp(k, "media_strip")) bad = parse_bool(v, &n.media_strip);
		else if (!strcmp(k, "media_reverse")) bad = parse_bool(v, &n.media_reverse);
		else if (!strcmp(k, "media_double_tap")) bad = parse_bool(v, &n.media_double_tap);
		else if (!strcmp(k, "bottom_height")) bad = parse_double(v, 0.02, 0.4, &n.bottom_height);
		else if (!strcmp(k, "osd")) bad = parse_bool(v, &n.osd);
		else if (!strcmp(k, "claim_mm")) bad = parse_double(v, 0.2, 20, &n.claim_mm);
		else if (!strcmp(k, "release_mm")) bad = parse_double(v, 0.5, 30, &n.release_mm);
		else if (!strcmp(k, "swipe_mm")) bad = parse_double(v, 2, 80, &n.swipe_mm);
		else if (!strcmp(k, "tap_ms")) bad = parse_int(v, 50, 1000, &n.tap_ms);
		else if (!strcmp(k, "double_tap_ms")) bad = parse_int(v, 100, 1500, &n.double_tap_ms);
		else if (!strcmp(k, "debug")) bad = parse_bool(v, &n.debug);
		else {
			fprintf(stderr, "omarchy-edges: %s:%d: unknown key '%s'\n", path, lineno, k);
			rc = -1;
			continue;
		}
		if (bad) {
			fprintf(stderr, "omarchy-edges: %s:%d: bad value for %s: '%s'\n", path, lineno, k, v);
			rc = -1;
		}
	}
	fclose(f);

	if (rc == 0)
		*c = n;
	return rc;
}
