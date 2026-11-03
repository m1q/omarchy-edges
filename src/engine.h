#ifndef EDGES_ENGINE_H
#define EDGES_ENGINE_H

/*
 * The gesture engine. It is pure logic: it eats evdev events from the
 * physical touchpad, decides which contacts belong to an edge gesture, and
 * produces (a) the filtered event stream for the virtual touchpad and
 * (b) high-level actions. No I/O happens here, so it can be unit tested.
 */

#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define EB_MAX_SLOTS 16
#define EB_MT_FIRST ABS_MT_TOUCH_MAJOR
#define EB_MT_LAST ABS_MT_TOOL_Y
#define EB_MT_COUNT (EB_MT_LAST - EB_MT_FIRST + 1)

typedef struct {
	int min_x, max_x, min_y, max_y;
	double res_x, res_y;    /* units per millimetre, never 0 */
	int nslots;
	bool buttonpad;
	bool has_abs[ABS_CNT];  /* what the virtual device can emit */
	bool has_key[KEY_CNT];
	bool has_msc_timestamp;
} eb_geometry;

enum eb_action_kind {
	EB_ACT_SLIDE_BEGIN,
	EB_ACT_SLIDE_UPDATE,
	EB_ACT_SLIDE_END,
	EB_ACT_MUTE_TOGGLE,
	EB_ACT_MEDIA_NEXT,
	EB_ACT_MEDIA_PREV,
	EB_ACT_MEDIA_PLAYPAUSE,
};

typedef struct {
	enum eb_action_kind kind;
	int channel;  /* for slides */
	double delta; /* for SLIDE_UPDATE: percent relative to the level at SLIDE_BEGIN */
} eb_action;

typedef void (*eb_emit_fn)(void *ctx, unsigned type, unsigned code, int value);
typedef void (*eb_action_fn)(void *ctx, const eb_action *a);

enum eb_owner {
	EB_OWN_NONE,    /* no contact */
	EB_OWN_PASS,    /* forwarded to the virtual touchpad */
	EB_OWN_PENDING, /* started on an edge, intent not known yet: held back */
	EB_OWN_SLIDER,  /* claimed by a side slider */
	EB_OWN_MEDIA,   /* claimed by the bottom media strip */
	EB_OWN_IGNORED, /* swallowed until lift (palm, cancelled slide) */
};

enum {
	EB_ZONE_LEFT = 1 << 0,
	EB_ZONE_RIGHT = 1 << 1,
	EB_ZONE_BOTTOM = 1 << 2, /* swipe for media */
	EB_ZONE_BOTTOM_TAP = 1 << 3,
	EB_ZONE_CORNER = 1 << 4, /* quick mute */
};

typedef struct {
	/* physical */
	bool active;
	bool began;   /* new contact this frame */
	int tid;
	int val[EB_MT_COUNT];
	uint64_t seq; /* begin order, for pointer emulation */

	/* gesture */
	enum eb_owner owner;
	unsigned zones;
	int sx, sy;
	uint64_t t0;
	double max_move_mm;
	int ref_y;
	int channel;
	bool fired;
	double last_delta;

	/* virtual */
	bool v_active;
	int v_tid;
	int v_val[EB_MT_COUNT];
} eb_slot;

typedef struct {
	eb_geometry g;
	eb_config c;
	eb_emit_fn emit;
	eb_action_fn act;
	void *ctx;

	eb_slot s[EB_MAX_SLOTS];
	int cur;           /* physical slot being updated */
	uint64_t now_us;
	uint64_t seq;

	int phys_btn[3];   /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE */
	int phys_tool;     /* finger count from BTN_TOOL_*, 0..5 */
	int phys_tool_key[5];
	bool btn_pressed_this_frame;
	int msc_timestamp;
	bool msc_timestamp_seen;

	/* virtual device state */
	int v_cur;
	int v_btn[3];
	int v_touch;
	int v_tool[5];
	int v_abs_x, v_abs_y, v_abs_p;

	/* double-tap tracking */
	unsigned last_tap_zone;
	uint64_t last_tap_end;

	unsigned long frames, frames_emitted;
} eb_engine;

void eb_engine_init(eb_engine *e, const eb_geometry *g, const eb_config *c,
		    eb_emit_fn emit, eb_action_fn act, void *ctx);
void eb_engine_set_config(eb_engine *e, const eb_config *c);
/* Seed the physical state of one slot (startup / resync). */
void eb_engine_seed_slot(eb_engine *e, int slot, int tid, const int *mt_values);
void eb_engine_event(eb_engine *e, const struct input_event *ev);
/* Lift every virtual contact and button, and end any slide. */
void eb_engine_release_all(eb_engine *e);
bool eb_engine_idle(const eb_engine *e);

#endif
