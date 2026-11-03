#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * omarchy-edgesd - turn the edges of a Linux touchpad into volume and brightness
 * sliders, with quick mute and a media strip.
 *
 * It grabs the physical touchpad (EVIOCGRAB), feeds every event through the
 * gesture engine, and re-emits the touches that are not edge gestures on a
 * virtual touchpad created with uinput. The compositor only ever sees the
 * virtual device, so a slide on an edge never moves the pointer.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include "actions.h"
#include "config.h"
#include "engine.h"

#define VIRT_NAME "Omarchy Edges Touchpad"
#define EB_VERSION "0.1.0"

static eb_config cfg;
static char cfg_path[1024];
static int exit_code;

/* ---- device discovery ------------------------------------------------- */

static bool is_touchpad(struct libevdev *d)
{
	return libevdev_has_event_code(d, EV_ABS, ABS_MT_POSITION_X) &&
	       libevdev_has_event_code(d, EV_ABS, ABS_MT_POSITION_Y) &&
	       libevdev_has_event_code(d, EV_ABS, ABS_MT_SLOT) &&
	       libevdev_has_event_code(d, EV_ABS, ABS_MT_TRACKING_ID) &&
	       libevdev_has_event_code(d, EV_KEY, BTN_TOOL_FINGER) &&
	       !libevdev_has_property(d, INPUT_PROP_DIRECT) &&
	       strcmp(libevdev_get_name(d), VIRT_NAME) != 0;
}

static int open_dev(const char *path, struct libevdev **out)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	int rc = libevdev_new_from_fd(fd, out);
	if (rc < 0) {
		close(fd);
		return rc;
	}
	return fd;
}

static int event_num(const char *name)
{
	return atoi(name + 5);
}

static int cmp_event(const struct dirent **a, const struct dirent **b)
{
	return event_num((*a)->d_name) - event_num((*b)->d_name);
}

static int filter_event(const struct dirent *d)
{
	return strncmp(d->d_name, "event", 5) == 0;
}

/* Find the first touchpad. Returns 0 and fills path, or -ENOENT / -EACCES. */
static int find_touchpad(char *path, size_t n, bool list)
{
	struct dirent **ents;
	int cnt = scandir("/dev/input", &ents, filter_event, cmp_event);
	if (cnt < 0)
		return -errno;
	int rc = -ENOENT;
	bool denied = false;
	for (int i = 0; i < cnt; i++) {
		char p[300];
		snprintf(p, sizeof(p), "/dev/input/%s", ents[i]->d_name);
		struct libevdev *d = NULL;
		int fd = open_dev(p, &d);
		if (fd == -EACCES || fd == -EPERM) {
			denied = true;
		} else if (fd >= 0) {
			bool tp = is_touchpad(d);
			if (list)
				printf("%-20s %s%s\n", p, libevdev_get_name(d), tp ? "  [touchpad]" : "");
			if (tp && rc != 0) {
				snprintf(path, n, "%s", p);
				rc = 0;
			}
			libevdev_free(d);
			close(fd);
		}
		free(ents[i]);
	}
	free(ents);
	if (rc != 0 && denied)
		rc = -EACCES;
	return rc;
}

static void fill_geometry(struct libevdev *d, eb_geometry *g)
{
	memset(g, 0, sizeof(*g));
	const struct input_absinfo *ax = libevdev_get_abs_info(d, ABS_MT_POSITION_X);
	const struct input_absinfo *ay = libevdev_get_abs_info(d, ABS_MT_POSITION_Y);
	g->min_x = ax->minimum;
	g->max_x = ax->maximum;
	g->min_y = ay->minimum;
	g->max_y = ay->maximum;
	g->res_x = ax->resolution;
	g->res_y = ay->resolution;
	g->nslots = libevdev_get_num_slots(d);
	g->buttonpad = libevdev_has_property(d, INPUT_PROP_BUTTONPAD);
	for (unsigned c = 0; c < ABS_CNT; c++)
		g->has_abs[c] = libevdev_has_event_code(d, EV_ABS, c);
	for (unsigned c = 0; c < KEY_CNT; c++)
		g->has_key[c] = libevdev_has_event_code(d, EV_KEY, c);
	g->has_msc_timestamp = libevdev_has_event_code(d, EV_MSC, MSC_TIMESTAMP);
}

static bool dev_idle(struct libevdev *d)
{
	int n = libevdev_get_num_slots(d);
	for (int s = 0; s < n; s++)
		if (libevdev_get_slot_value(d, s, ABS_MT_TRACKING_ID) >= 0)
			return false;
	return !libevdev_get_event_value(d, EV_KEY, BTN_LEFT) &&
	       !libevdev_get_event_value(d, EV_KEY, BTN_RIGHT) &&
	       !libevdev_get_event_value(d, EV_KEY, BTN_MIDDLE);
}

/* ---- engine glue ------------------------------------------------------ */

static struct libevdev_uinput *uidev;
static unsigned long write_errors;

static void emit_cb(void *ctx, unsigned type, unsigned code, int value)
{
	(void)ctx;
	if (libevdev_uinput_write_event(uidev, type, code, value) < 0 && write_errors++ < 5)
		fprintf(stderr, "omarchy-edges: uinput write failed: %s\n", strerror(errno));
}

static void action_cb(void *ctx, const eb_action *a)
{
	(void)ctx;
	eb_actions_post(a);
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static long long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void reload_config(eb_engine *e)
{
	eb_config n;
	eb_config_defaults(&n);
	if (eb_config_load(&n, cfg_path) < 0) {
		fprintf(stderr, "omarchy-edges: keeping the previous config\n");
		return;
	}
	n.debug = n.debug || cfg.debug;
	char dev[sizeof(cfg.device)];
	snprintf(dev, sizeof(dev), "%s", cfg.device);
	cfg = n;
	snprintf(cfg.device, sizeof(cfg.device), "%s", dev); /* device changes need a restart */
	if (e)
		eb_engine_set_config(e, &cfg);
	eb_actions_set_config(&cfg);
	fprintf(stderr, "omarchy-edges: config reloaded\n");
}

enum run_result { RUN_STOP, RUN_RETRY };

/* Drain events until the device has no contacts. Returns false on a signal to stop. */
static int wait_idle(struct libevdev *d, int fd, int sfd)
{
	bool told = false;
	while (!dev_idle(d)) {
		if (!told) {
			fprintf(stderr, "omarchy-edges: waiting for the touchpad to be released\n");
			told = true;
		}
		struct pollfd p[2] = { { .fd = fd, .events = POLLIN }, { .fd = sfd, .events = POLLIN } };
		if (poll(p, 2, -1) < 0 && errno != EINTR)
			return -1;
		if (p[1].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof(si)) == sizeof(si) && si.ssi_signo != SIGHUP)
				return 0;
		}
		struct input_event ev;
		int rc;
		do {
			rc = libevdev_next_event(d, LIBEVDEV_READ_FLAG_NORMAL, &ev);
			while (rc == LIBEVDEV_READ_STATUS_SYNC)
				rc = libevdev_next_event(d, LIBEVDEV_READ_FLAG_SYNC, &ev);
		} while (rc == LIBEVDEV_READ_STATUS_SUCCESS);
		if (rc != -EAGAIN)
			return -1;
	}
	return 1;
}

static enum run_result run_device(const char *path, int sfd)
{
	struct libevdev *d = NULL;
	int fd = open_dev(path, &d);
	if (fd < 0) {
		fprintf(stderr, "omarchy-edges: cannot open %s: %s\n", path, strerror(-fd));
		return RUN_RETRY;
	}
	if (!is_touchpad(d)) {
		fprintf(stderr, "omarchy-edges: %s (%s) is not a touchpad\n", path, libevdev_get_name(d));
		libevdev_free(d);
		close(fd);
		exit_code = 1;
		return RUN_STOP;
	}
	libevdev_set_clock_id(d, CLOCK_MONOTONIC);

	eb_geometry g;
	fill_geometry(d, &g);
	char phys_name[256];
	snprintf(phys_name, sizeof(phys_name), "%s", libevdev_get_name(d));

	libevdev_set_name(d, VIRT_NAME);
	int rc = libevdev_uinput_create_from_device(d, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
	libevdev_set_name(d, phys_name);
	if (rc < 0) {
		fprintf(stderr, "omarchy-edges: cannot create the virtual touchpad: %s\n", strerror(-rc));
		libevdev_free(d);
		close(fd);
		return RUN_RETRY;
	}

	enum run_result result = RUN_RETRY;
	int w = wait_idle(d, fd, sfd);
	if (w <= 0) {
		result = w == 0 ? RUN_STOP : RUN_RETRY;
		goto out_uinput;
	}
	rc = libevdev_grab(d, LIBEVDEV_GRAB);
	if (rc < 0) {
		fprintf(stderr, "omarchy-edges: cannot grab %s: %s%s\n", path, strerror(-rc),
			rc == -EBUSY ? " (another program, such as edgepad, already has it)" : "");
		goto out_uinput;
	}

	double wmm = (g.max_x - g.min_x) / (g.res_x > 0 ? g.res_x : 1);
	double hmm = (g.max_y - g.min_y) / (g.res_y > 0 ? g.res_y : 1);
	fprintf(stderr, "omarchy-edges: %s (%s), %.0fx%.0f mm, %d slots%s -> %s\n", path, phys_name, wmm, hmm,
		g.nslots, g.buttonpad ? ", clickpad" : "", libevdev_uinput_get_devnode(uidev) ?: "?");
	fprintf(stderr, "omarchy-edges: left=%s right=%s edge=%.0f%% quick_mute=%s media_strip=%s\n",
		eb_channel_name(cfg.left), eb_channel_name(cfg.right), cfg.edge_width * 100,
		cfg.quick_mute ? "on" : "off", cfg.media_strip ? "on" : "off");

	static eb_engine e;
	eb_engine_init(&e, &g, &cfg, emit_cb, action_cb, NULL);
	for (int s = 0; s < g.nslots && s < EB_MAX_SLOTS; s++) {
		int vals[EB_MT_COUNT];
		for (unsigned c = EB_MT_FIRST; c <= EB_MT_LAST; c++)
			vals[c - EB_MT_FIRST] = libevdev_get_slot_value(d, s, c);
		eb_engine_seed_slot(&e, s, libevdev_get_slot_value(d, s, ABS_MT_TRACKING_ID), vals);
	}

	bool stopping = false;
	long long stop_deadline = 0;
	for (;;) {
		int timeout = -1;
		if (stopping) {
			long long left = stop_deadline - mono_ms();
			if (left <= 0 || eb_engine_idle(&e)) {
				result = RUN_STOP;
				break;
			}
			timeout = (int)left;
		}
		struct pollfd p[2] = { { .fd = fd, .events = POLLIN }, { .fd = sfd, .events = POLLIN } };
		int pr = poll(p, 2, timeout);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "omarchy-edges: poll: %s\n", strerror(errno));
			break;
		}
		if (p[1].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof(si)) == sizeof(si)) {
				if (si.ssi_signo == SIGHUP) {
					reload_config(&e);
				} else if (!stopping) {
					stopping = true;
					stop_deadline = mono_ms() + 500; /* let a touch finish first */
				}
			}
		}
		if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "omarchy-edges: touchpad went away\n");
			break;
		}
		if (!(p[0].revents & POLLIN))
			continue;

		struct input_event ev;
		do {
			rc = libevdev_next_event(d, LIBEVDEV_READ_FLAG_NORMAL, &ev);
			if (rc == LIBEVDEV_READ_STATUS_SYNC) {
				if (cfg.debug)
					fprintf(stderr, "omarchy-edges: SYN_DROPPED, resyncing\n");
				/* libevdev replays the difference as ordinary events. */
				while (rc == LIBEVDEV_READ_STATUS_SYNC) {
					rc = libevdev_next_event(d, LIBEVDEV_READ_FLAG_SYNC, &ev);
					if (rc == LIBEVDEV_READ_STATUS_SYNC)
						eb_engine_event(&e, &ev);
				}
				rc = LIBEVDEV_READ_STATUS_SUCCESS;
				continue;
			}
			if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
				eb_engine_event(&e, &ev);
		} while (rc == LIBEVDEV_READ_STATUS_SUCCESS);

		if (rc != -EAGAIN) {
			fprintf(stderr, "omarchy-edges: reading %s: %s\n", path, strerror(-rc));
			break;
		}
	}

	eb_engine_release_all(&e);
	libevdev_grab(d, LIBEVDEV_UNGRAB);
out_uinput:
	/* Give the compositor a moment to see the released state. */
	sleep_ms(30);
	libevdev_uinput_destroy(uidev);
	uidev = NULL;
	libevdev_free(d);
	close(fd);
	return result;
}

/* ---- main ------------------------------------------------------------- */

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: omarchy-edgesd [options]\n"
		"  -c, --config FILE   config file (default ~/.config/omarchy-edges/edges.conf)\n"
		"  -d, --device PATH   touchpad event device (default: auto)\n"
		"  -l, --list          list input devices and exit\n"
		"      --check         print the detected touchpad and zones, then exit\n"
		"  -v, --debug         log gesture decisions\n"
		"  -V, --version       print the version\n");
}

static int check(const char *path)
{
	struct libevdev *d = NULL;
	int fd = open_dev(path, &d);
	if (fd < 0) {
		fprintf(stderr, "omarchy-edges: cannot open %s: %s\n", path, strerror(-fd));
		return 1;
	}
	eb_geometry g;
	fill_geometry(d, &g);
	double rx = g.res_x > 0 ? g.res_x : 1, ry = g.res_y > 0 ? g.res_y : 1;
	double wmm = (g.max_x - g.min_x) / rx, hmm = (g.max_y - g.min_y) / ry;
	printf("device:      %s\n", path);
	printf("name:        %s\n", libevdev_get_name(d));
	printf("touchpad:    %s\n", is_touchpad(d) ? "yes" : "NO");
	printf("x range:     %d..%d (%.0f units/mm%s)\n", g.min_x, g.max_x, g.res_x, g.res_x > 0 ? "" : ", unknown");
	printf("y range:     %d..%d (%.0f units/mm%s)\n", g.min_y, g.max_y, g.res_y, g.res_y > 0 ? "" : ", unknown");
	printf("size:        %.0f x %.0f mm\n", wmm, hmm);
	printf("slots:       %d\n", g.nslots);
	printf("clickpad:    %s\n", g.buttonpad ? "yes" : "no");
	printf("palm type:   %s\n", g.has_abs[ABS_MT_TOOL_TYPE] ? "reported" : "not reported");
	printf("left band:   %s, %.1f mm wide\n", eb_channel_name(cfg.left), wmm * cfg.edge_width);
	printf("right band:  %s, %.1f mm wide\n", eb_channel_name(cfg.right), wmm * cfg.edge_width);
	printf("bottom band: %s, %.1f mm tall\n", cfg.media_strip ? "media" : "off", hmm * cfg.bottom_height);
	printf("full sweep:  %.0f mm of travel = 0..100%%\n", hmm * cfg.travel);
	libevdev_free(d);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "device", required_argument, NULL, 'd' },
		{ "list", no_argument, NULL, 'l' },
		{ "check", no_argument, NULL, 'C' },
		{ "debug", no_argument, NULL, 'v' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	const char *opt_device = NULL;
	bool opt_list = false, opt_check = false, opt_debug = false;

	cfg_path[0] = '\0';
	int o;
	while ((o = getopt_long(argc, argv, "c:d:lvVh", opts, NULL)) != -1) {
		switch (o) {
		case 'c': snprintf(cfg_path, sizeof(cfg_path), "%s", optarg); break;
		case 'd': opt_device = optarg; break;
		case 'l': opt_list = true; break;
		case 'C': opt_check = true; break;
		case 'v': opt_debug = true; break;
		case 'V': printf("omarchy-edgesd %s\n", EB_VERSION); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}

	if (!cfg_path[0]) {
		const char *xdg = getenv("XDG_CONFIG_HOME");
		const char *home = getenv("HOME");
		if (xdg && *xdg)
			snprintf(cfg_path, sizeof(cfg_path), "%s/omarchy-edges/edges.conf", xdg);
		else
			snprintf(cfg_path, sizeof(cfg_path), "%s/.config/omarchy-edges/edges.conf", home ? home : ".");
	}
	eb_config_defaults(&cfg);
	if (eb_config_load(&cfg, cfg_path) < 0) {
		fprintf(stderr, "omarchy-edges: fix %s and try again\n", cfg_path);
		return 1;
	}
	if (opt_device)
		snprintf(cfg.device, sizeof(cfg.device), "%s", opt_device);
	if (opt_debug)
		cfg.debug = true;

	if (opt_list) {
		char p[300];
		int rc = find_touchpad(p, sizeof(p), true);
		if (rc == -EACCES)
			fprintf(stderr, "omarchy-edges: some devices are not readable; install the udev rule (see README)\n");
		return rc == 0 ? 0 : 1;
	}

	char path[300];
	bool auto_dev = strcmp(cfg.device, "auto") == 0;

	if (opt_check) {
		if (auto_dev) {
			int rc = find_touchpad(path, sizeof(path), false);
			if (rc < 0) {
				fprintf(stderr, "omarchy-edges: no touchpad found%s\n",
					rc == -EACCES ? " (permission denied: install the udev rule)" : "");
				return 1;
			}
		} else {
			snprintf(path, sizeof(path), "%s", cfg.device);
		}
		return check(path);
	}

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL); /* before threads start, so they inherit it */
	signal(SIGPIPE, SIG_IGN);
	int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sfd < 0) {
		perror("omarchy-edges: signalfd");
		return 1;
	}

	if (eb_actions_start(&cfg) < 0) {
		fprintf(stderr, "omarchy-edges: cannot start worker threads\n");
		return 1;
	}

	bool told_missing = false;
	for (;;) {
		if (auto_dev) {
			int fr = find_touchpad(path, sizeof(path), false);
			if (fr < 0) {
				if (!told_missing) {
					fprintf(stderr, "omarchy-edges: no touchpad found%s; waiting\n",
						fr == -EACCES ? " (permission denied: install the udev rule)" : "");
					told_missing = true;
				}
				goto wait;
			}
		} else {
			snprintf(path, sizeof(path), "%s", cfg.device);
		}
		told_missing = false;

		if (run_device(path, sfd) == RUN_STOP)
			break;
	wait:;
		/* Retry in a second, but stay responsive to signals. */
		struct pollfd p = { .fd = sfd, .events = POLLIN };
		if (poll(&p, 1, 1000) > 0) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof(si)) == sizeof(si)) {
				if (si.ssi_signo == SIGHUP)
					reload_config(NULL);
				else
					break;
			}
		}
	}

	eb_actions_stop();
	close(sfd);
	return exit_code;
}
