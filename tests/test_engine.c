/* Unit tests for the gesture engine: feed synthetic touchpad frames, check
 * what reaches the virtual touchpad and which actions fire. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/engine.h"

/* A pad shaped like a laptop clickpad: 1200 x 700 units, 12 units/mm
 * (100 x 58 mm), 5 slots, pressure and palm reporting. */
#define W 1200
#define H 700
#define RES 12

typedef struct { unsigned type, code; int value; } ev_t;

static ev_t out[4096];
static int nout;
static eb_action acts[256];
static int nacts;
static uint64_t now_us = 1000000;
static int failures, checks;

static void emit(void *ctx, unsigned type, unsigned code, int value)
{
	(void)ctx;
	if (nout < (int)(sizeof(out) / sizeof(out[0])))
		out[nout++] = (ev_t){ type, code, value };
}

static void on_action(void *ctx, const eb_action *a)
{
	(void)ctx;
	if (nacts < 256)
		acts[nacts++] = *a;
}

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static eb_engine e;
static eb_config cfg;
static int fingers;

static void setup(void)
{
	eb_geometry g;
	memset(&g, 0, sizeof(g));
	g.min_x = 0; g.max_x = W; g.min_y = 0; g.max_y = H;
	g.res_x = RES; g.res_y = RES;
	g.nslots = 5;
	g.buttonpad = true;
	unsigned abs_codes[] = { ABS_X, ABS_Y, ABS_PRESSURE, ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
				 ABS_MT_TRACKING_ID, ABS_MT_PRESSURE, ABS_MT_TOOL_TYPE };
	for (size_t i = 0; i < sizeof(abs_codes) / sizeof(abs_codes[0]); i++)
		g.has_abs[abs_codes[i]] = true;
	unsigned key_codes[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
				 BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
	for (size_t i = 0; i < sizeof(key_codes) / sizeof(key_codes[0]); i++)
		g.has_key[key_codes[i]] = true;

	eb_config_defaults(&cfg);
	eb_engine_init(&e, &g, &cfg, emit, on_action, NULL);
	nout = nacts = 0;
	fingers = 0;
	now_us += 10 * 1000 * 1000;
}

static void reconfig(void)
{
	eb_engine_set_config(&e, &cfg);
}

static void ev(unsigned type, unsigned code, int value)
{
	struct input_event ie;
	memset(&ie, 0, sizeof(ie));
	ie.input_event_sec = (time_t)(now_us / 1000000);
	ie.input_event_usec = (suseconds_t)(now_us % 1000000);
	ie.type = (unsigned short)type;
	ie.code = (unsigned short)code;
	ie.value = value;
	eb_engine_event(&e, &ie);
}

static int tool_for(int n)
{
	static const unsigned t[] = { BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
				      BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
	return (int)t[(n < 1 ? 1 : n > 5 ? 5 : n) - 1];
}

static void down(int slot, int tid, int x, int y)
{
	ev(EV_ABS, ABS_MT_SLOT, slot);
	ev(EV_ABS, ABS_MT_TRACKING_ID, tid);
	ev(EV_ABS, ABS_MT_POSITION_X, x);
	ev(EV_ABS, ABS_MT_POSITION_Y, y);
	ev(EV_ABS, ABS_MT_PRESSURE, 40);
	if (fingers)
		ev(EV_KEY, (unsigned)tool_for(fingers), 0);
	fingers++;
	ev(EV_KEY, (unsigned)tool_for(fingers), 1);
	if (fingers == 1)
		ev(EV_KEY, BTN_TOUCH, 1);
}

static void move(int slot, int x, int y)
{
	ev(EV_ABS, ABS_MT_SLOT, slot);
	ev(EV_ABS, ABS_MT_POSITION_X, x);
	ev(EV_ABS, ABS_MT_POSITION_Y, y);
}

static void up(int slot)
{
	ev(EV_ABS, ABS_MT_SLOT, slot);
	ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
	ev(EV_KEY, (unsigned)tool_for(fingers), 0);
	fingers--;
	if (fingers)
		ev(EV_KEY, (unsigned)tool_for(fingers), 1);
	else
		ev(EV_KEY, BTN_TOUCH, 0);
}

static void syn(int ms)
{
	ev(EV_SYN, SYN_REPORT, 0);
	now_us += (uint64_t)ms * 1000;
}

static int find(unsigned type, unsigned code, int value)
{
	for (int i = 0; i < nout; i++)
		if (out[i].type == type && out[i].code == code && out[i].value == value)
			return i;
	return -1;
}

static int count_code(unsigned type, unsigned code)
{
	int n = 0;
	for (int i = 0; i < nout; i++)
		if (out[i].type == type && out[i].code == code)
			n++;
	return n;
}

static int count_act(enum eb_action_kind k)
{
	int n = 0;
	for (int i = 0; i < nacts; i++)
		if (acts[i].kind == k)
			n++;
	return n;
}

static const eb_action *last_act(enum eb_action_kind k)
{
	for (int i = nacts - 1; i >= 0; i--)
		if (acts[i].kind == k)
			return &acts[i];
	return NULL;
}

static void clear(void)
{
	nout = nacts = 0;
}

/* mm -> units */
#define MM(x) ((int)((x) * RES))

/* ---------------------------------------------------------------------- */

static void test_center_touch_passes_through(void)
{
	printf("center touch passes through\n");
	setup();
	down(0, 10, 600, 350);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 10) >= 0, "tracking id forwarded");
	CHECK(find(EV_ABS, ABS_MT_POSITION_X, 600) >= 0, "x forwarded");
	CHECK(find(EV_KEY, BTN_TOUCH, 1) >= 0, "BTN_TOUCH down");
	CHECK(find(EV_KEY, BTN_TOOL_FINGER, 1) >= 0, "one finger");
	CHECK(out[nout - 1].type == EV_SYN, "frame ends with SYN_REPORT");
	clear();
	move(0, 650, 360);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_POSITION_X, 650) >= 0, "motion forwarded");
	CHECK(find(EV_ABS, ABS_X, 650) >= 0, "pointer emulation follows");
	clear();
	up(0);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, -1) >= 0, "lift forwarded");
	CHECK(find(EV_KEY, BTN_TOUCH, 0) >= 0, "BTN_TOUCH up");
	CHECK(nacts == 0, "no actions (%d)", nacts);
}

static void test_right_edge_slide_volume(void)
{
	printf("right edge slide drives volume and never moves the pointer\n");
	setup();
	int x = W - 20;
	down(0, 11, x, 500);
	syn(10);
	CHECK(nout == 0, "edge touch held back (%d events)", nout);
	for (int i = 1; i <= 10; i++) {
		move(0, x, 500 - MM(2) * i);
		syn(10);
	}
	CHECK(nout == 0, "slide never reaches the virtual touchpad (%d events)", nout);
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 1, "one slide begin");
	const eb_action *b = last_act(EB_ACT_SLIDE_BEGIN);
	CHECK(b && b->channel == EB_CH_VOLUME, "right edge = volume");
	const eb_action *u = last_act(EB_ACT_SLIDE_UPDATE);
	CHECK(u && u->delta > 0, "moving up raises the level");
	/* 20 mm of travel minus the 2 mm claim, on 58 mm * 0.8 = 46.7 mm full sweep */
	double expect = MM(18) / (H * 0.8) * 100.0;
	CHECK(u && fabs(u->delta - expect) < 0.5, "delta %.2f ~ %.2f", u ? u->delta : -1, expect);
	/* Moving back to where the slide engaged gives the level back. */
	move(0, x, 500 - MM(2));
	syn(10);
	u = last_act(EB_ACT_SLIDE_UPDATE);
	CHECK(u && fabs(u->delta) < 0.01, "returning to the start gives delta 0 (%.2f)", u ? u->delta : -1);
	up(0);
	syn(10);
	CHECK(count_act(EB_ACT_SLIDE_END) == 1, "slide end on lift");
	CHECK(nout == 0, "lift not forwarded (%d)", nout);
}

static void test_left_edge_slide_brightness_down(void)
{
	printf("left edge slide down lowers brightness\n");
	setup();
	down(0, 12, 15, 200);
	syn(10);
	for (int i = 1; i <= 5; i++) {
		move(0, 15, 200 + MM(2) * i);
		syn(10);
	}
	const eb_action *b = last_act(EB_ACT_SLIDE_BEGIN);
	CHECK(b && b->channel == EB_CH_BRIGHTNESS, "left edge = brightness");
	const eb_action *u = last_act(EB_ACT_SLIDE_UPDATE);
	CHECK(u && u->delta < 0, "moving down lowers the level");
	CHECK(nout == 0, "nothing forwarded");
}

static void test_edge_touch_moving_inward_is_pointer(void)
{
	printf("edge touch that moves inward becomes pointer motion\n");
	setup();
	down(0, 13, W - 20, 300);
	syn(10);
	move(0, W - 20 - MM(1), 300);
	syn(10);
	CHECK(nout == 0, "still undecided after 1 mm");
	move(0, W - 20 - MM(4), 300);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 13) >= 0, "promoted to the virtual touchpad");
	CHECK(find(EV_KEY, BTN_TOUCH, 1) >= 0, "touch begins on the virtual touchpad");
	clear();
	move(0, W - 20 - MM(10), 300);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_POSITION_X, W - 20 - MM(10)) >= 0, "keeps moving the pointer");
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 0, "no slide");
	up(0);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, -1) >= 0, "lift forwarded");
}

static void test_second_finger_hands_back(void)
{
	printf("second finger turns an edge touch into a normal two-finger gesture\n");
	setup();
	down(0, 14, W - 20, 300);
	syn(10);
	CHECK(nout == 0, "held back");
	down(1, 15, W - 200, 300);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 14) >= 0, "first finger released to libinput");
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 15) >= 0, "second finger forwarded");
	CHECK(find(EV_KEY, BTN_TOOL_DOUBLETAP, 1) >= 0, "two fingers reported");
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 0, "no slide");
}

static void test_touch_on_edge_during_pointer_motion(void)
{
	printf("an edge touch while another finger is down passes through\n");
	setup();
	down(0, 16, 600, 350);
	syn(10);
	clear();
	down(1, 17, W - 15, 350);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 17) >= 0, "forwarded immediately");
	move(1, W - 15, 350 - MM(10));
	syn(10);
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 0, "two-finger scroll is not a slide");
}

static void test_click_on_edge_promotes_before_button(void)
{
	printf("a physical click on an edge reaches libinput with its finger\n");
	setup();
	down(0, 18, W - 20, 650);
	syn(10);
	CHECK(nout == 0, "held back");
	ev(EV_KEY, BTN_LEFT, 1);
	syn(10);
	int t = find(EV_ABS, ABS_MT_TRACKING_ID, 18), b = find(EV_KEY, BTN_LEFT, 1);
	CHECK(t >= 0 && b >= 0 && t < b, "contact (%d) goes out before the button (%d)", t, b);
	clear();
	ev(EV_KEY, BTN_LEFT, 0);
	syn(10);
	CHECK(find(EV_KEY, BTN_LEFT, 0) >= 0, "release forwarded");
}

static void test_click_during_slide(void)
{
	printf("a click while sliding ends the slide and clicks\n");
	setup();
	down(0, 19, W - 20, 500);
	syn(10);
	move(0, W - 20, 500 - MM(5));
	syn(10);
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 1, "sliding");
	ev(EV_KEY, BTN_LEFT, 1);
	syn(10);
	CHECK(count_act(EB_ACT_SLIDE_END) == 1, "slide ended");
	CHECK(find(EV_KEY, BTN_LEFT, 1) >= 0, "click forwarded");
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 19) >= 0, "with its finger");
}

static void tap(int slot, int tid, int x, int y, int hold_ms)
{
	down(slot, tid, x, y);
	syn(hold_ms);
	up(slot);
	syn(10);
}

static void test_corner_double_tap_mutes(void)
{
	printf("double-tap in the top-right corner toggles mute\n");
	setup();
	tap(0, 20, W - 30, 30, 60);
	CHECK(count_act(EB_ACT_MUTE_TOGGLE) == 0, "one tap does nothing");
	now_us += 100 * 1000;
	tap(0, 21, W - 32, 35, 60);
	CHECK(count_act(EB_ACT_MUTE_TOGGLE) == 1, "second tap mutes");
	CHECK(nout == 0, "taps are swallowed (%d events)", nout);
	now_us += 100 * 1000;
	tap(0, 22, W - 30, 30, 60);
	CHECK(count_act(EB_ACT_MUTE_TOGGLE) == 1, "a third tap starts a new double-tap");
}

static void test_corner_taps_too_slow(void)
{
	printf("two corner taps far apart do not mute\n");
	setup();
	tap(0, 23, W - 30, 30, 60);
	now_us += 600 * 1000;
	tap(0, 24, W - 30, 30, 60);
	CHECK(count_act(EB_ACT_MUTE_TOGGLE) == 0, "no mute");
}

static void test_corner_long_press_not_a_tap(void)
{
	printf("holding the corner is not a tap\n");
	setup();
	tap(0, 25, W - 30, 30, 400);
	now_us += 50 * 1000;
	tap(0, 26, W - 30, 30, 400);
	CHECK(count_act(EB_ACT_MUTE_TOGGLE) == 0, "no mute");
}

static void test_bottom_swipe_skips(void)
{
	printf("swipe along the bottom edge skips tracks\n");
	setup();
	int y = H - 15;
	down(0, 30, 400, y);
	syn(10);
	for (int i = 1; i <= 8; i++) {
		move(0, 400 + MM(2) * i, y);
		syn(10);
	}
	CHECK(count_act(EB_ACT_MEDIA_NEXT) == 1, "swipe right = next, once (%d)", count_act(EB_ACT_MEDIA_NEXT));
	CHECK(nout == 0, "swipe never reaches the virtual touchpad");
	up(0);
	syn(10);

	down(0, 31, 800, y);
	syn(10);
	for (int i = 1; i <= 8; i++) {
		move(0, 800 - MM(2) * i, y);
		syn(10);
	}
	CHECK(count_act(EB_ACT_MEDIA_PREV) == 1, "swipe left = previous");
	up(0);
	syn(10);

	cfg.media_reverse = true;
	reconfig();
	down(0, 32, 400, y);
	syn(10);
	move(0, 400 + MM(15), y);
	syn(10);
	CHECK(count_act(EB_ACT_MEDIA_PREV) == 2, "reversed: swipe right = previous");
}

static void test_bottom_upward_is_pointer(void)
{
	printf("a touch starting on the bottom edge and moving up is pointer motion\n");
	setup();
	down(0, 33, 600, H - 10);
	syn(10);
	move(0, 600, H - 10 - MM(5));
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 33) >= 0, "promoted");
	CHECK(count_act(EB_ACT_MEDIA_NEXT) + count_act(EB_ACT_MEDIA_PREV) == 0, "no media action");
}

static void test_bottom_right_corner_both_ways(void)
{
	printf("bottom-right corner: vertical = volume, horizontal = media\n");
	setup();
	down(0, 34, W - 20, H - 15);
	syn(10);
	move(0, W - 20, H - 15 - MM(6));
	syn(10);
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 1, "vertical slides volume");
	up(0);
	syn(10);
	down(0, 35, W - 20, H - 15);
	syn(10);
	move(0, W - 20 - MM(14), H - 15);
	syn(10);
	CHECK(count_act(EB_ACT_MEDIA_PREV) == 1, "horizontal skips back");
}

static void test_double_tap_play_pause(void)
{
	printf("double-tap on the bottom strip plays/pauses when enabled\n");
	setup();
	tap(0, 40, 600, H - 10, 50);
	now_us += 100 * 1000;
	tap(0, 41, 600, H - 10, 50);
	CHECK(count_act(EB_ACT_MEDIA_PLAYPAUSE) == 0, "off by default");
	cfg.media_double_tap = true;
	reconfig();
	now_us += 1000 * 1000;
	tap(0, 42, 600, H - 10, 50);
	now_us += 100 * 1000;
	tap(0, 43, 600, H - 10, 50);
	CHECK(count_act(EB_ACT_MEDIA_PLAYPAUSE) == 1, "on: plays/pauses");
}

static void test_palm_on_edge_ignored(void)
{
	printf("a palm on the edge is ignored\n");
	setup();
	down(0, 50, W - 20, 400);
	syn(10);
	ev(EV_ABS, ABS_MT_SLOT, 0);
	ev(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM);
	move(0, W - 20, 400 - MM(10));
	syn(10);
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 0, "no slide");
	CHECK(nout == 0, "not forwarded");
}

static void test_active_area(void)
{
	printf("active_area keeps slides to the lower part of the pad\n");
	setup();
	cfg.active_area = 0.5;
	reconfig();
	down(0, 60, W - 20, 200); /* upper half */
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 60) >= 0, "upper half passes through");
	up(0);
	syn(10);
	clear();
	down(0, 61, W - 20, 600); /* lower half */
	syn(10);
	move(0, W - 20, 600 - MM(5));
	syn(10);
	CHECK(count_act(EB_ACT_SLIDE_BEGIN) == 1, "lower half slides");
}

static void test_swap_and_disable(void)
{
	printf("sides can be swapped or turned off\n");
	setup();
	cfg.left = EB_CH_VOLUME;
	cfg.right = EB_CH_NONE;
	cfg.quick_mute = false;
	reconfig();
	down(0, 70, W - 20, 400);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 70) >= 0, "right edge off: passes through");
	up(0);
	syn(10);
	down(0, 71, 20, 400);
	syn(10);
	move(0, 20, 400 - MM(5));
	syn(10);
	const eb_action *b = last_act(EB_ACT_SLIDE_BEGIN);
	CHECK(b && b->channel == EB_CH_VOLUME, "left edge = volume");
}

static void test_slot_reuse(void)
{
	printf("a new tracking id in the same slot is a new contact\n");
	setup();
	down(0, 80, 600, 350);
	syn(10);
	clear();
	/* Kernel reports a new contact in slot 0 without a -1 first. */
	ev(EV_ABS, ABS_MT_SLOT, 0);
	ev(EV_ABS, ABS_MT_TRACKING_ID, 81);
	ev(EV_ABS, ABS_MT_POSITION_X, W - 20);
	ev(EV_ABS, ABS_MT_POSITION_Y, 400);
	syn(10);
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, -1) >= 0, "old contact lifted on the virtual device");
	CHECK(find(EV_ABS, ABS_MT_TRACKING_ID, 81) < 0, "new edge contact held back");
}

static void test_release_all(void)
{
	printf("release_all lifts everything\n");
	setup();
	down(0, 90, 600, 350);
	down(1, 91, 700, 350);
	syn(10);
	ev(EV_KEY, BTN_LEFT, 1);
	syn(10);
	clear();
	eb_engine_release_all(&e);
	CHECK(count_code(EV_ABS, ABS_MT_TRACKING_ID) == 2, "both contacts lifted");
	CHECK(find(EV_KEY, BTN_LEFT, 0) >= 0, "button released");
	CHECK(find(EV_KEY, BTN_TOUCH, 0) >= 0, "touch released");
	CHECK(out[nout - 1].type == EV_SYN, "frame closed");
}

static void test_three_fingers_center(void)
{
	printf("three-finger gestures pass through untouched\n");
	setup();
	down(0, 100, 500, 300);
	down(1, 101, 600, 300);
	down(2, 102, 700, 300);
	syn(10);
	CHECK(find(EV_KEY, BTN_TOOL_TRIPLETAP, 1) >= 0, "three fingers");
	CHECK(count_code(EV_ABS, ABS_MT_TRACKING_ID) == 3, "three contacts");
}

int main(void)
{
	test_center_touch_passes_through();
	test_right_edge_slide_volume();
	test_left_edge_slide_brightness_down();
	test_edge_touch_moving_inward_is_pointer();
	test_second_finger_hands_back();
	test_touch_on_edge_during_pointer_motion();
	test_click_on_edge_promotes_before_button();
	test_click_during_slide();
	test_corner_double_tap_mutes();
	test_corner_taps_too_slow();
	test_corner_long_press_not_a_tap();
	test_bottom_swipe_skips();
	test_bottom_upward_is_pointer();
	test_bottom_right_corner_both_ways();
	test_double_tap_play_pause();
	test_palm_on_edge_ignored();
	test_active_area();
	test_swap_and_disable();
	test_slot_reuse();
	test_release_all();
	test_three_fingers_center();
	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
