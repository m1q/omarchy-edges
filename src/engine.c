#include "engine.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DBG(e, ...) do { if ((e)->c.debug) fprintf(stderr, "omarchy-edges: " __VA_ARGS__); } while (0)

static const unsigned btn_codes[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
static const unsigned tool_codes[5] = {
	BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP,
};

static const char *owner_name(enum eb_owner o)
{
	switch (o) {
	case EB_OWN_NONE: return "none";
	case EB_OWN_PASS: return "pass";
	case EB_OWN_PENDING: return "pending";
	case EB_OWN_SLIDER: return "slider";
	case EB_OWN_MEDIA: return "media";
	case EB_OWN_IGNORED: return "ignored";
	}
	return "?";
}

static inline int mt(const eb_slot *s, unsigned code)
{
	return s->val[code - EB_MT_FIRST];
}

static inline bool is_palm(const eb_engine *e, const eb_slot *s)
{
	return e->g.has_abs[ABS_MT_TOOL_TYPE] && mt(s, ABS_MT_TOOL_TYPE) == MT_TOOL_PALM;
}

static void act(eb_engine *e, enum eb_action_kind kind, int channel, double delta)
{
	eb_action a = { .kind = kind, .channel = channel, .delta = delta };
	if (e->act)
		e->act(e->ctx, &a);
}

void eb_engine_init(eb_engine *e, const eb_geometry *g, const eb_config *c,
		    eb_emit_fn emit, eb_action_fn actfn, void *ctx)
{
	memset(e, 0, sizeof(*e));
	e->g = *g;
	if (e->g.nslots > EB_MAX_SLOTS)
		e->g.nslots = EB_MAX_SLOTS;
	if (e->g.nslots < 1)
		e->g.nslots = 1;
	if (e->g.res_x <= 0)
		e->g.res_x = (e->g.max_x - e->g.min_x) / 100.0;
	if (e->g.res_y <= 0)
		e->g.res_y = (e->g.max_y - e->g.min_y) / 60.0;
	if (e->g.res_x <= 0) e->g.res_x = 1;
	if (e->g.res_y <= 0) e->g.res_y = 1;
	e->c = *c;
	e->emit = emit;
	e->act = actfn;
	e->ctx = ctx;
	for (int i = 0; i < EB_MAX_SLOTS; i++) {
		e->s[i].tid = -1;
		e->s[i].v_tid = -1;
		e->s[i].channel = EB_CH_NONE;
	}
	e->v_cur = -1;
	e->v_abs_x = e->v_abs_y = e->v_abs_p = INT_MIN;
}

void eb_engine_set_config(eb_engine *e, const eb_config *c)
{
	e->c = *c;
}

void eb_engine_seed_slot(eb_engine *e, int slot, int tid, const int *mt_values)
{
	if (slot < 0 || slot >= e->g.nslots)
		return;
	eb_slot *s = &e->s[slot];
	if (mt_values)
		memcpy(s->val, mt_values, sizeof(s->val));
	s->tid = tid;
	s->active = tid >= 0;
	/* We do not know the history of a contact that was already down:
	 * keep it away from both the gestures and the virtual device. */
	s->owner = s->active ? EB_OWN_IGNORED : EB_OWN_NONE;
}

/* ---- zones ------------------------------------------------------------ */

static unsigned zones_for(const eb_engine *e, int x, int y)
{
	const eb_config *c = &e->c;
	double w = e->g.max_x - e->g.min_x, h = e->g.max_y - e->g.min_y;
	if (w <= 0 || h <= 0)
		return 0;
	double nx = (x - e->g.min_x) / w, ny = (y - e->g.min_y) / h;
	unsigned z = 0;

	if (c->left != EB_CH_NONE && nx <= c->edge_width && ny >= 1.0 - c->active_area)
		z |= EB_ZONE_LEFT;
	if (c->right != EB_CH_NONE && nx >= 1.0 - c->edge_width && ny >= 1.0 - c->active_area)
		z |= EB_ZONE_RIGHT;
	if ((z & EB_ZONE_LEFT) && (z & EB_ZONE_RIGHT))
		z &= nx < 0.5 ? ~(unsigned)EB_ZONE_RIGHT : ~(unsigned)EB_ZONE_LEFT;

	bool bottom = ny >= 1.0 - c->bottom_height;
	if (c->media_strip && bottom)
		z |= EB_ZONE_BOTTOM;
	if (c->media_double_tap && bottom)
		z |= EB_ZONE_BOTTOM_TAP;

	double cw = c->edge_width > 0.08 ? c->edge_width : 0.08;
	if (c->quick_mute && nx >= 1.0 - cw && ny <= 0.15)
		z |= EB_ZONE_CORNER;
	return z;
}

/* ---- contact lifecycle ------------------------------------------------ */

static void set_owner(eb_engine *e, int i, enum eb_owner o, const char *why)
{
	eb_slot *s = &e->s[i];
	if (s->owner != o)
		DBG(e, "slot %d tid %d: %s -> %s (%s)\n", i, s->tid, owner_name(s->owner), owner_name(o), why);
	s->owner = o;
}

/* Hand a held-back contact to the virtual touchpad. */
static void promote(eb_engine *e, int i, const char *why)
{
	eb_slot *s = &e->s[i];
	if (s->owner == EB_OWN_SLIDER)
		act(e, EB_ACT_SLIDE_END, s->channel, 0);
	set_owner(e, i, EB_OWN_PASS, why);
}

static void handle_tap(eb_engine *e, eb_slot *s)
{
	unsigned z = 0;
	if (s->zones & EB_ZONE_CORNER)
		z = EB_ZONE_CORNER;
	else if (s->zones & EB_ZONE_BOTTOM_TAP)
		z = EB_ZONE_BOTTOM_TAP;
	if (!z)
		return;

	uint64_t gap_limit = (uint64_t)e->c.double_tap_ms * 1000;
	if (e->last_tap_zone == z && s->t0 >= e->last_tap_end && s->t0 - e->last_tap_end <= gap_limit) {
		e->last_tap_zone = 0;
		if (z == EB_ZONE_CORNER) {
			DBG(e, "double-tap corner: mute\n");
			act(e, EB_ACT_MUTE_TOGGLE, EB_CH_VOLUME, 0);
		} else {
			DBG(e, "double-tap bottom: play/pause\n");
			act(e, EB_ACT_MEDIA_PLAYPAUSE, EB_CH_NONE, 0);
		}
	} else {
		e->last_tap_zone = z;
		e->last_tap_end = e->now_us;
	}
}

static void contact_end(eb_engine *e, int i)
{
	eb_slot *s = &e->s[i];
	switch (s->owner) {
	case EB_OWN_PENDING:
		if (e->now_us - s->t0 <= (uint64_t)e->c.tap_ms * 1000 && s->max_move_mm < e->c.claim_mm)
			handle_tap(e, s);
		break;
	case EB_OWN_SLIDER:
		act(e, EB_ACT_SLIDE_END, s->channel, 0);
		break;
	default:
		break;
	}
	if (s->owner != EB_OWN_NONE)
		DBG(e, "slot %d tid %d: lift (%s)\n", i, s->tid, owner_name(s->owner));
	s->active = false;
	s->began = false;
	s->owner = EB_OWN_NONE;
	s->channel = EB_CH_NONE;
}

/* ---- virtual output --------------------------------------------------- */

static int out_count;

static void out(eb_engine *e, unsigned type, unsigned code, int value)
{
	e->emit(e->ctx, type, code, value);
	out_count++;
}

static void vslot(eb_engine *e, int i)
{
	if (e->v_cur != i) {
		out(e, EV_ABS, ABS_MT_SLOT, i);
		e->v_cur = i;
	}
}

static void emit_frame(eb_engine *e, bool releasing)
{
	out_count = 0;

	for (int i = 0; i < e->g.nslots; i++) {
		eb_slot *s = &e->s[i];
		bool want = !releasing && s->active && s->owner == EB_OWN_PASS;

		if (want && (!s->v_active || s->v_tid != s->tid)) {
			vslot(e, i);
			out(e, EV_ABS, ABS_MT_TRACKING_ID, s->tid);
			s->v_active = true;
			s->v_tid = s->tid;
			for (unsigned code = EB_MT_FIRST; code <= EB_MT_LAST; code++) {
				if (code == ABS_MT_TRACKING_ID || !e->g.has_abs[code])
					continue;
				out(e, EV_ABS, code, mt(s, code));
				s->v_val[code - EB_MT_FIRST] = mt(s, code);
			}
		} else if (want) {
			for (unsigned code = EB_MT_FIRST; code <= EB_MT_LAST; code++) {
				if (code == ABS_MT_TRACKING_ID || !e->g.has_abs[code])
					continue;
				if (s->v_val[code - EB_MT_FIRST] != mt(s, code)) {
					vslot(e, i);
					out(e, EV_ABS, code, mt(s, code));
					s->v_val[code - EB_MT_FIRST] = mt(s, code);
				}
			}
		} else if (s->v_active) {
			vslot(e, i);
			out(e, EV_ABS, ABS_MT_TRACKING_ID, -1);
			s->v_active = false;
			s->v_tid = -1;
		}
	}

	/* Finger count. Synthesised from what the virtual device sees, except
	 * when nothing is held back and the hardware reports more fingers than
	 * it has slots for. */
	int vcount = 0;
	bool suppressed = false;
	eb_slot *oldest = NULL;
	for (int i = 0; i < e->g.nslots; i++) {
		eb_slot *s = &e->s[i];
		if (s->v_active) {
			vcount++;
			if (!oldest || s->seq < oldest->seq)
				oldest = s;
		}
		if (s->active && s->owner != EB_OWN_PASS)
			suppressed = true;
	}
	int count = vcount;
	if (!releasing && !suppressed && e->phys_tool > count)
		count = e->phys_tool;

	for (int t = 0; t < 5; t++) {
		int want = count > 0 && (count >= 5 ? t == 4 : t == count - 1);
		if (e->g.has_key[tool_codes[t]] && e->v_tool[t] != want) {
			out(e, EV_KEY, tool_codes[t], want);
			e->v_tool[t] = want;
		}
	}
	int touch = count > 0;
	if (e->g.has_key[BTN_TOUCH] && e->v_touch != touch) {
		out(e, EV_KEY, BTN_TOUCH, touch);
		e->v_touch = touch;
	}

	/* Single-touch pointer emulation follows the oldest forwarded contact. */
	if (oldest) {
		if (e->g.has_abs[ABS_X] && e->v_abs_x != mt(oldest, ABS_MT_POSITION_X)) {
			e->v_abs_x = mt(oldest, ABS_MT_POSITION_X);
			out(e, EV_ABS, ABS_X, e->v_abs_x);
		}
		if (e->g.has_abs[ABS_Y] && e->v_abs_y != mt(oldest, ABS_MT_POSITION_Y)) {
			e->v_abs_y = mt(oldest, ABS_MT_POSITION_Y);
			out(e, EV_ABS, ABS_Y, e->v_abs_y);
		}
		if (e->g.has_abs[ABS_PRESSURE] && e->g.has_abs[ABS_MT_PRESSURE] &&
		    e->v_abs_p != mt(oldest, ABS_MT_PRESSURE)) {
			e->v_abs_p = mt(oldest, ABS_MT_PRESSURE);
			out(e, EV_ABS, ABS_PRESSURE, e->v_abs_p);
		}
	} else if (e->g.has_abs[ABS_PRESSURE] && e->v_abs_p != 0 && e->v_abs_p != INT_MIN) {
		e->v_abs_p = 0;
		out(e, EV_ABS, ABS_PRESSURE, 0);
	}

	/* Physical buttons come after the contacts, so a contact promoted by a
	 * click is already down when libinput sees the button. */
	for (int b = 0; b < 3; b++) {
		int want = releasing ? 0 : e->phys_btn[b];
		if (e->g.has_key[btn_codes[b]] && e->v_btn[b] != want) {
			out(e, EV_KEY, btn_codes[b], want);
			e->v_btn[b] = want;
		}
	}

	if (out_count) {
		if (e->msc_timestamp_seen && e->g.has_msc_timestamp)
			e->emit(e->ctx, EV_MSC, MSC_TIMESTAMP, e->msc_timestamp);
		e->emit(e->ctx, EV_SYN, SYN_REPORT, 0);
		e->frames_emitted++;
	}
}

/* ---- frame processing ------------------------------------------------- */

static int others_live(const eb_engine *e, int self)
{
	int n = 0;
	for (int i = 0; i < e->g.nslots; i++)
		if (i != self && e->s[i].active && e->s[i].owner != EB_OWN_IGNORED)
			n++;
	return n;
}

static void begin_contact(eb_engine *e, int i, bool button_held)
{
	eb_slot *s = &e->s[i];
	s->sx = mt(s, ABS_MT_POSITION_X);
	s->sy = mt(s, ABS_MT_POSITION_Y);
	s->t0 = e->now_us;
	s->max_move_mm = 0;
	s->fired = false;
	s->channel = EB_CH_NONE;
	s->last_delta = 0;
	s->zones = zones_for(e, s->sx, s->sy);

	bool eligible = s->zones && !button_held && !is_palm(e, s) && others_live(e, i) == 0;
	s->owner = EB_OWN_NONE;
	set_owner(e, i, eligible ? EB_OWN_PENDING : EB_OWN_PASS, eligible ? "edge touch" : "touch");
	if (eligible)
		DBG(e, "slot %d: zones 0x%x at %d,%d\n", i, s->zones, s->sx, s->sy);
}

static void update_contact(eb_engine *e, int i)
{
	eb_slot *s = &e->s[i];
	int x = mt(s, ABS_MT_POSITION_X), y = mt(s, ABS_MT_POSITION_Y);
	double dx = (x - s->sx) / e->g.res_x, dy = (y - s->sy) / e->g.res_y;
	double adx = fabs(dx), ady = fabs(dy), dist = hypot(dx, dy);
	if (dist > s->max_move_mm)
		s->max_move_mm = dist;

	switch (s->owner) {
	case EB_OWN_PENDING:
		if (is_palm(e, s)) {
			set_owner(e, i, EB_OWN_IGNORED, "palm");
		} else if ((s->zones & (EB_ZONE_LEFT | EB_ZONE_RIGHT)) && ady >= e->c.claim_mm && ady >= 1.5 * adx) {
			s->channel = (s->zones & EB_ZONE_LEFT) ? e->c.left : e->c.right;
			s->ref_y = y;
			s->last_delta = 0;
			set_owner(e, i, EB_OWN_SLIDER, eb_channel_name(s->channel));
			act(e, EB_ACT_SLIDE_BEGIN, s->channel, 0);
		} else if ((s->zones & EB_ZONE_BOTTOM) && adx >= e->c.claim_mm && adx >= 1.5 * ady) {
			set_owner(e, i, EB_OWN_MEDIA, "swipe");
		} else if (dist >= e->c.release_mm) {
			promote(e, i, "moved away");
		}
		break;
	case EB_OWN_SLIDER:
		if (is_palm(e, s)) {
			act(e, EB_ACT_SLIDE_END, s->channel, 0);
			set_owner(e, i, EB_OWN_IGNORED, "palm");
			break;
		}
		{
			double span = (e->g.max_y - e->g.min_y) * e->c.travel;
			double delta = span > 0 ? (s->ref_y - y) / span * 100.0 : 0;
			if (fabs(delta - s->last_delta) > 0.01) {
				s->last_delta = delta;
				act(e, EB_ACT_SLIDE_UPDATE, s->channel, delta);
			}
		}
		break;
	default:
		break;
	}

	if (s->owner == EB_OWN_MEDIA && !s->fired && adx >= e->c.swipe_mm) {
		s->fired = true;
		bool next = (dx > 0) != e->c.media_reverse;
		DBG(e, "media swipe: %s\n", next ? "next" : "previous");
		act(e, next ? EB_ACT_MEDIA_NEXT : EB_ACT_MEDIA_PREV, EB_CH_NONE, 0);
	}
}

static void frame(eb_engine *e)
{
	e->frames++;

	bool button_held = e->phys_btn[0] || e->phys_btn[1] || e->phys_btn[2];

	/* A click on a clickpad needs the finger that clicks: give every
	 * held-back contact to libinput before the button goes out. */
	if (e->btn_pressed_this_frame && e->g.buttonpad) {
		for (int i = 0; i < e->g.nslots; i++) {
			enum eb_owner o = e->s[i].owner;
			if (e->s[i].active && (o == EB_OWN_PENDING || o == EB_OWN_SLIDER || o == EB_OWN_MEDIA))
				promote(e, i, "click");
		}
	}

	bool any_began = false;
	for (int i = 0; i < e->g.nslots; i++) {
		if (e->s[i].active && e->s[i].began) {
			e->s[i].began = false;
			any_began = true;
			begin_contact(e, i, button_held);
		}
	}

	/* A second finger means a multi-finger gesture: an edge touch that
	 * has not committed yet belongs to it. */
	if (any_began) {
		for (int i = 0; i < e->g.nslots; i++)
			if (e->s[i].active && e->s[i].owner == EB_OWN_PENDING && others_live(e, i) > 0)
				promote(e, i, "second finger");
	}

	for (int i = 0; i < e->g.nslots; i++)
		if (e->s[i].active)
			update_contact(e, i);

	emit_frame(e, false);

	e->btn_pressed_this_frame = false;
	e->msc_timestamp_seen = false;
}

void eb_engine_event(eb_engine *e, const struct input_event *ev)
{
	e->now_us = (uint64_t)ev->input_event_sec * 1000000ull + (uint64_t)ev->input_event_usec;

	switch (ev->type) {
	case EV_ABS:
		if (ev->code == ABS_MT_SLOT) {
			e->cur = ev->value;
			return;
		}
		if (e->cur < 0 || e->cur >= e->g.nslots)
			return;
		if (ev->code == ABS_MT_TRACKING_ID) {
			eb_slot *s = &e->s[e->cur];
			if (ev->value < 0) {
				if (s->active)
					contact_end(e, e->cur);
				s->tid = -1;
			} else if (!s->active || s->tid != ev->value) {
				if (s->active)
					contact_end(e, e->cur);
				s->active = true;
				s->began = true;
				s->tid = ev->value;
				s->seq = ++e->seq;
			}
			return;
		}
		if (ev->code >= EB_MT_FIRST && ev->code <= EB_MT_LAST)
			e->s[e->cur].val[ev->code - EB_MT_FIRST] = ev->value;
		/* Legacy ABS_X/Y/PRESSURE are synthesised on output. */
		return;

	case EV_KEY:
		for (int b = 0; b < 3; b++) {
			if (ev->code == btn_codes[b]) {
				if (ev->value && !e->phys_btn[b])
					e->btn_pressed_this_frame = true;
				e->phys_btn[b] = ev->value != 0;
				return;
			}
		}
		for (int t = 0; t < 5; t++) {
			if (ev->code == tool_codes[t]) {
				e->phys_tool_key[t] = ev->value != 0;
				e->phys_tool = 0;
				for (int k = 4; k >= 0; k--)
					if (e->phys_tool_key[k]) {
						e->phys_tool = k + 1;
						break;
					}
				return;
			}
		}
		return;

	case EV_MSC:
		if (ev->code == MSC_TIMESTAMP) {
			e->msc_timestamp = ev->value;
			e->msc_timestamp_seen = true;
		}
		return;

	case EV_SYN:
		if (ev->code == SYN_REPORT)
			frame(e);
		return;

	default:
		return;
	}
}

void eb_engine_release_all(eb_engine *e)
{
	for (int i = 0; i < e->g.nslots; i++) {
		eb_slot *s = &e->s[i];
		if (s->owner == EB_OWN_SLIDER)
			act(e, EB_ACT_SLIDE_END, s->channel, 0);
		if (s->active)
			s->owner = EB_OWN_IGNORED;
	}
	emit_frame(e, true);
}

bool eb_engine_idle(const eb_engine *e)
{
	for (int i = 0; i < e->g.nslots; i++)
		if (e->s[i].active)
			return false;
	return !e->phys_btn[0] && !e->phys_btn[1] && !e->phys_btn[2];
}
