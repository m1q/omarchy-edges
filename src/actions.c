#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "actions.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define CMD_TIMEOUT_MS 3000

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ctl_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t osd_cv = PTHREAD_COND_INITIALIZER;
static pthread_t ctl_thread, osd_thread;
static bool stopping;
static eb_config cfg;

static struct {
	unsigned gen;
	double delta;
	bool dirty;
} chan[EB_CH_COUNT];

#define QMAX 16
static enum eb_action_kind queue[QMAX];
static int queue_n;

static struct {
	bool dirty;
	char icon[32];
	int value;
} osd;

static struct {
	bool omarchy_osd, omarchy_sink, omarchy_volume, omarchy_shell, omarchy_display;
	bool pactl, brightnessctl, playerctl;
} have;

/* ---- process helpers -------------------------------------------------- */

static bool have_cmd(const char *name)
{
	const char *path = getenv("PATH");
	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	char buf[4096];
	const char *p = path;
	while (*p) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		if (len && len + strlen(name) + 2 < sizeof(buf)) {
			memcpy(buf, p, len);
			buf[len] = '/';
			strcpy(buf + len + 1, name);
			struct stat st;
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0)
				return true;
		}
		if (!colon)
			break;
		p = colon + 1;
	}
	return false;
}

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Run argv without a shell. Captures stdout into out (if given). Returns the
 * exit status, or -1 if the command could not run or timed out. */
static int run(char *const argv[], char *out, size_t outsz)
{
	int pipefd[2] = { -1, -1 };
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	sigset_t none, all;

	if (out && pipe2(pipefd, O_CLOEXEC) < 0)
		return -1;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	if (out)
		posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
	else
		posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);

	/* The daemon blocks signals for its signalfd; children must not inherit that. */
	posix_spawnattr_init(&at);
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &all);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

	pid_t pid;
	int rc = posix_spawnp(&pid, argv[0], &fa, &at, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	if (out)
		close(pipefd[1]);
	if (rc != 0) {
		if (out)
			close(pipefd[0]);
		return -1;
	}

	long long deadline = now_ms() + CMD_TIMEOUT_MS;
	if (out) {
		size_t n = 0;
		for (;;) {
			long long left = deadline - now_ms();
			if (left <= 0)
				break;
			struct pollfd p = { .fd = pipefd[0], .events = POLLIN };
			int pr = poll(&p, 1, (int)left);
			if (pr < 0 && errno == EINTR)
				continue;
			if (pr <= 0)
				break;
			char tmp[256];
			ssize_t r = read(pipefd[0], tmp, sizeof(tmp));
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0)
				break;
			size_t take = (size_t)r;
			if (n + take > outsz - 1)
				take = outsz - 1 - n;
			memcpy(out + n, tmp, take);
			n += take;
		}
		out[n] = '\0';
		close(pipefd[0]);
	}

	int status;
	for (;;) {
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid)
			break;
		if (w < 0 && errno != EINTR)
			return -1;
		if (now_ms() > deadline) {
			fprintf(stderr, "omarchy-edges: '%s' timed out\n", argv[0]);
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return -1;
		}
		struct timespec ts = { 0, 2 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void chomp(char *s)
{
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

/* ---- volume ----------------------------------------------------------- */

static char sink[256];

static void resolve_sink(void)
{
	sink[0] = '\0';
	if (have.omarchy_sink) {
		char *argv[] = { "omarchy-audio-output-sink", NULL };
		char buf[256];
		if (run(argv, buf, sizeof(buf)) == 0) {
			chomp(buf);
			if (buf[0] && !strchr(buf, '\n'))
				snprintf(sink, sizeof(sink), "%s", buf);
		}
	}
	if (!sink[0])
		snprintf(sink, sizeof(sink), "@DEFAULT_SINK@");
}

static int get_volume(bool *muted)
{
	char buf[1024];
	char *argv[] = { "pactl", "get-sink-volume", sink, NULL };
	*muted = false;
	if (run(argv, buf, sizeof(buf)) != 0)
		return -1;
	/* "Volume: front-left: 65536 / 100% / 0.00 dB, ..." */
	char *pct = strchr(buf, '%');
	if (!pct)
		return -1;
	char *p = pct;
	while (p > buf && isdigit((unsigned char)p[-1]))
		p--;
	if (p == pct)
		return -1;
	int v = atoi(p);

	char mbuf[128];
	char *margv[] = { "pactl", "get-sink-mute", sink, NULL };
	if (run(margv, mbuf, sizeof(mbuf)) == 0 && strstr(mbuf, "yes"))
		*muted = true;
	return v;
}

static void set_volume(int v)
{
	char arg[16];
	snprintf(arg, sizeof(arg), "%d%%", v);
	char *argv[] = { "pactl", "set-sink-volume", sink, arg, NULL };
	run(argv, NULL, 0);
}

static void set_mute(bool m)
{
	char *argv[] = { "pactl", "set-sink-mute", sink, m ? "1" : "0", NULL };
	run(argv, NULL, 0);
}

/* ---- brightness ------------------------------------------------------- */

static char backlight[128];
static bool backlight_resolved;

static void resolve_backlight(void)
{
	if (backlight_resolved)
		return;
	backlight_resolved = true;
	backlight[0] = '\0';
	if (have.omarchy_display) {
		char *argv[] = { "omarchy-hw-display", NULL };
		char buf[128];
		if (run(argv, buf, sizeof(buf)) == 0) {
			chomp(buf);
			if (buf[0] && !strchr(buf, '\n') && !strchr(buf, '/'))
				snprintf(backlight, sizeof(backlight), "%s", buf);
		}
	}
}

static int get_brightness(void)
{
	resolve_backlight();
	char buf[256];
	char *argv_dev[] = { "brightnessctl", "-d", backlight, "-m", NULL };
	char *argv_def[] = { "brightnessctl", "-m", NULL };
	if (run(backlight[0] ? argv_dev : argv_def, buf, sizeof(buf)) != 0)
		return -1;
	/* "intel_backlight,backlight,64000,100%,64000" */
	char *f = buf;
	for (int i = 0; i < 3 && f; i++) {
		f = strchr(f, ',');
		if (f)
			f++;
	}
	if (!f || !isdigit((unsigned char)*f))
		return -1;
	return atoi(f);
}

static void set_brightness(int v)
{
	char arg[16];
	snprintf(arg, sizeof(arg), "%d%%", v);
	char *argv_dev[] = { "brightnessctl", "-q", "-d", backlight, "set", arg, NULL };
	char *argv_def[] = { "brightnessctl", "-q", "set", arg, NULL };
	run(backlight[0] ? argv_dev : argv_def, NULL, 0);
}

/* ---- OSD -------------------------------------------------------------- */

static void post_osd(const char *icon, int value)
{
	if (!have.omarchy_osd)
		return;
	pthread_mutex_lock(&mu);
	snprintf(osd.icon, sizeof(osd.icon), "%s", icon);
	osd.value = value;
	osd.dirty = true;
	pthread_cond_signal(&osd_cv);
	pthread_mutex_unlock(&mu);
}

static void *osd_main(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&mu);
		while (!stopping && !osd.dirty)
			pthread_cond_wait(&osd_cv, &mu);
		if (stopping) {
			pthread_mutex_unlock(&mu);
			return NULL;
		}
		char icon[32], val[16];
		snprintf(icon, sizeof(icon), "%s", osd.icon);
		snprintf(val, sizeof(val), "%d", osd.value);
		osd.dirty = false;
		pthread_mutex_unlock(&mu);

		char *argv[] = { "omarchy-osd", "-i", icon, "-p", val, NULL };
		run(argv, NULL, 0);
	}
}

/* ---- one-shot actions ------------------------------------------------- */

static void oneshot(enum eb_action_kind k)
{
	switch (k) {
	case EB_ACT_MUTE_TOGGLE:
		if (have.omarchy_volume) {
			char *argv[] = { "omarchy-audio-output-volume", "mute-toggle", NULL };
			run(argv, NULL, 0);
		} else {
			char *argv[] = { "pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL };
			run(argv, NULL, 0);
		}
		break;
	case EB_ACT_MEDIA_NEXT:
	case EB_ACT_MEDIA_PREV:
	case EB_ACT_MEDIA_PLAYPAUSE: {
		const char *shell_m = k == EB_ACT_MEDIA_NEXT ? "next" : k == EB_ACT_MEDIA_PREV ? "previous" : "playPause";
		const char *pctl_m = k == EB_ACT_MEDIA_NEXT ? "next" : k == EB_ACT_MEDIA_PREV ? "previous" : "play-pause";
		if (have.omarchy_shell) {
			char *argv[] = { "omarchy-shell", "-q", "media", (char *)shell_m, NULL };
			run(argv, NULL, 0);
		} else if (have.playerctl) {
			char *argv[] = { "playerctl", (char *)pctl_m, NULL };
			run(argv, NULL, 0);
		}
		break;
	}
	default:
		break;
	}
}

/* ---- control thread --------------------------------------------------- */

static void *ctl_main(void *arg)
{
	(void)arg;
	unsigned seen[EB_CH_COUNT] = { 0 };
	int base[EB_CH_COUNT] = { 0 }, last[EB_CH_COUNT] = { 0 };
	bool valid[EB_CH_COUNT] = { false };
	bool muted = false;

	for (;;) {
		pthread_mutex_lock(&mu);
		while (!stopping && !queue_n && !chan[0].dirty && !chan[1].dirty)
			pthread_cond_wait(&ctl_cv, &mu);
		if (stopping) {
			pthread_mutex_unlock(&mu);
			return NULL;
		}
		enum eb_action_kind q[QMAX];
		int qn = queue_n;
		memcpy(q, queue, sizeof(q[0]) * (size_t)qn);
		queue_n = 0;
		unsigned gen[EB_CH_COUNT];
		double delta[EB_CH_COUNT];
		bool dirty[EB_CH_COUNT];
		for (int c = 0; c < EB_CH_COUNT; c++) {
			gen[c] = chan[c].gen;
			delta[c] = chan[c].delta;
			dirty[c] = chan[c].dirty;
			chan[c].dirty = false;
		}
		eb_config c = cfg;
		pthread_mutex_unlock(&mu);

		for (int i = 0; i < qn; i++)
			oneshot(q[i]);

		for (int ch = 0; ch < EB_CH_COUNT; ch++) {
			if (!dirty[ch])
				continue;
			if (gen[ch] != seen[ch]) {
				/* A new slide starts from the current system level. */
				seen[ch] = gen[ch];
				if (ch == EB_CH_VOLUME) {
					resolve_sink();
					base[ch] = have.pactl ? get_volume(&muted) : -1;
				} else {
					base[ch] = have.brightnessctl ? get_brightness() : -1;
				}
				valid[ch] = base[ch] >= 0;
				last[ch] = base[ch];
				if (!valid[ch])
					fprintf(stderr, "omarchy-edges: could not read the current %s\n", eb_channel_name(ch));
				else if (c.osd)
					post_osd(ch == EB_CH_BRIGHTNESS ? "brightness" : (muted || base[ch] == 0) ? "volume-muted" : "volume-high",
						 base[ch]);
			}
			if (!valid[ch])
				continue;

			int lo = ch == EB_CH_BRIGHTNESS ? 1 : 0;
			int step = c.step > 0 ? c.step : 1;
			long target = base[ch] + lround(delta[ch] / step) * step;
			if (target < lo) target = lo;
			if (target > 100) target = 100;
			if (target == last[ch])
				continue;
			last[ch] = (int)target;

			if (ch == EB_CH_VOLUME) {
				if (muted) {
					set_mute(false);
					muted = false;
				}
				set_volume((int)target);
				if (c.osd)
					post_osd(target == 0 ? "volume-muted" : "volume-high", (int)target);
			} else {
				set_brightness((int)target);
				if (c.osd)
					post_osd("brightness", (int)target);
			}
			if (c.debug)
				fprintf(stderr, "omarchy-edges: %s -> %ld\n", eb_channel_name(ch), target);
		}
	}
}

/* ---- API -------------------------------------------------------------- */

int eb_actions_start(const eb_config *c)
{
	cfg = *c;
	have.omarchy_osd = have_cmd("omarchy-osd");
	have.omarchy_sink = have_cmd("omarchy-audio-output-sink");
	have.omarchy_volume = have_cmd("omarchy-audio-output-volume");
	have.omarchy_shell = have_cmd("omarchy-shell");
	have.omarchy_display = have_cmd("omarchy-hw-display");
	have.pactl = have_cmd("pactl");
	have.brightnessctl = have_cmd("brightnessctl");
	have.playerctl = have_cmd("playerctl");

	if (!have.pactl)
		fprintf(stderr, "omarchy-edges: pactl not found, volume slider disabled\n");
	if (!have.brightnessctl)
		fprintf(stderr, "omarchy-edges: brightnessctl not found, brightness slider disabled\n");
	if (!have.omarchy_osd)
		fprintf(stderr, "omarchy-edges: omarchy-osd not found, no on-screen display\n");

	stopping = false;
	if (pthread_create(&ctl_thread, NULL, ctl_main, NULL) != 0)
		return -1;
	if (pthread_create(&osd_thread, NULL, osd_main, NULL) != 0)
		return -1;
	return 0;
}

void eb_actions_set_config(const eb_config *c)
{
	pthread_mutex_lock(&mu);
	cfg = *c;
	pthread_mutex_unlock(&mu);
}

void eb_actions_post(const eb_action *a)
{
	pthread_mutex_lock(&mu);
	switch (a->kind) {
	case EB_ACT_SLIDE_BEGIN:
		if (a->channel >= 0 && a->channel < EB_CH_COUNT) {
			chan[a->channel].gen++;
			chan[a->channel].delta = 0;
			chan[a->channel].dirty = true;
		}
		break;
	case EB_ACT_SLIDE_UPDATE:
		if (a->channel >= 0 && a->channel < EB_CH_COUNT) {
			chan[a->channel].delta = a->delta;
			chan[a->channel].dirty = true;
		}
		break;
	case EB_ACT_SLIDE_END:
		break;
	default:
		if (queue_n < QMAX)
			queue[queue_n++] = a->kind;
		break;
	}
	pthread_cond_signal(&ctl_cv);
	pthread_mutex_unlock(&mu);
}

void eb_actions_stop(void)
{
	pthread_mutex_lock(&mu);
	stopping = true;
	pthread_cond_broadcast(&ctl_cv);
	pthread_cond_broadcast(&osd_cv);
	pthread_mutex_unlock(&mu);
	pthread_join(ctl_thread, NULL);
	pthread_join(osd_thread, NULL);
}
