#ifndef EDGES_CONFIG_H
#define EDGES_CONFIG_H

#include <stdbool.h>

enum eb_channel {
	EB_CH_NONE = -1,
	EB_CH_VOLUME = 0,
	EB_CH_BRIGHTNESS = 1,
	EB_CH_COUNT = 2,
};

typedef struct {
	char device[256];      /* "auto" or /dev/input/eventN */

	int left;              /* enum eb_channel */
	int right;             /* enum eb_channel */
	double edge_width;     /* side band width, fraction of pad width */
	double active_area;    /* side slides may start in this bottom fraction of the pad */
	double travel;         /* fraction of pad height that sweeps 0..100% */
	int step;              /* level granularity in percent */

	bool quick_mute;       /* double-tap the top-right corner */
	bool media_strip;      /* swipe the bottom band to skip tracks */
	bool media_reverse;    /* swipe left for next instead of right */
	bool media_double_tap; /* double-tap the bottom band to play/pause */
	double bottom_height;  /* bottom band height, fraction of pad height */

	bool osd;              /* show the Omarchy OSD while sliding */

	/* Gesture tuning, in millimetres and milliseconds. */
	double claim_mm;       /* movement along the band that turns a touch into a slide */
	double release_mm;     /* movement in any other direction that hands the touch back */
	double swipe_mm;       /* bottom-band travel that skips a track */
	int tap_ms;            /* longest touch that still counts as a tap */
	int double_tap_ms;     /* longest gap between the two taps of a double-tap */

	bool debug;
} eb_config;

void eb_config_defaults(eb_config *c);
/* Returns 0 on success (a missing file is not an error), -1 on a parse error. */
int eb_config_load(eb_config *c, const char *path);
const char *eb_channel_name(int ch);

#endif
