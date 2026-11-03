/*
 * End-to-end test through the real kernel: creates a fake clickpad with
 * uinput, runs omarchy-edgesd on it, injects touches and reads what comes out of
 * the "Omarchy Edges Touchpad" virtual device. Needs the udev rule installed.
 *
 *   make e2e && ./tests/e2e ./omarchy-edgesd
 *
 * It only uses gestures without side effects (no volume or brightness
 * changes). The forwarded centre touches can nudge the real pointer.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#define FAKE_VENDOR 0x1d6b
#define FAKE_PRODUCT 0xeb01
#define W 1200
#define H 700

static struct libevdev_uinput *fake;
static struct libevdev *virt;
static int virt_fd = -1;
static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
	else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static void w(unsigned type, unsigned code, int value)
{
	libevdev_uinput_write_event(fake, type, code, value);
}

static void syn(void)
{
	w(EV_SYN, SYN_REPORT, 0);
	msleep(8);
}

static void make_fake(void)
{
	struct libevdev *d = libevdev_new();
	libevdev_set_name(d, "Omarchy Edges Test Pad");
	libevdev_set_id_bustype(d, BUS_I2C);
	libevdev_set_id_vendor(d, FAKE_VENDOR);
	libevdev_set_id_product(d, FAKE_PRODUCT);
	libevdev_enable_property(d, INPUT_PROP_POINTER);
	libevdev_enable_property(d, INPUT_PROP_BUTTONPAD);
	struct input_absinfo ax = { .minimum = 0, .maximum = W, .resolution = 12 };
	struct input_absinfo ay = { .minimum = 0, .maximum = H, .resolution = 12 };
	struct input_absinfo slot = { .minimum = 0, .maximum = 4 };
	struct input_absinfo tid = { .minimum = 0, .maximum = 65535 };
	struct input_absinfo pr = { .minimum = 0, .maximum = 255 };
	struct input_absinfo tool = { .minimum = 0, .maximum = 2 };
	libevdev_enable_event_code(d, EV_ABS, ABS_X, &ax);
	libevdev_enable_event_code(d, EV_ABS, ABS_Y, &ay);
	libevdev_enable_event_code(d, EV_ABS, ABS_PRESSURE, &pr);
	libevdev_enable_event_code(d, EV_ABS, ABS_MT_SLOT, &slot);
	libevdev_enable_event_code(d, EV_ABS, ABS_MT_POSITION_X, &ax);
	libevdev_enable_event_code(d, EV_ABS, ABS_MT_POSITION_Y, &ay);
	libevdev_enable_event_code(d, EV_ABS, ABS_MT_TRACKING_ID, &tid);
	libevdev_enable_event_code(d, EV_ABS, ABS_MT_PRESSURE, &pr);
	libevdev_enable_event_code(d, EV_ABS, ABS_MT_TOOL_TYPE, &tool);
	unsigned keys[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
			    BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		libevdev_enable_event_code(d, EV_KEY, keys[i], NULL);
	int rc = libevdev_uinput_create_from_device(d, LIBEVDEV_UINPUT_OPEN_MANAGED, &fake);
	libevdev_free(d);
	if (rc < 0) {
		fprintf(stderr, "cannot create the fake pad: %s\n", strerror(-rc));
		exit(2);
	}
}

static int find_virtual(void)
{
	DIR *dir = opendir("/dev/input");
	struct dirent *de;
	while (dir && (de = readdir(dir))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		char p[300];
		snprintf(p, sizeof(p), "/dev/input/%s", de->d_name);
		int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		struct libevdev *d;
		if (libevdev_new_from_fd(fd, &d) == 0) {
			if (!strcmp(libevdev_get_name(d), "Omarchy Edges Touchpad") &&
			    libevdev_get_id_product(d) == FAKE_PRODUCT) {
				virt = d;
				virt_fd = fd;
				closedir(dir);
				return 0;
			}
			libevdev_free(d);
		}
		close(fd);
	}
	if (dir)
		closedir(dir);
	return -1;
}

/* Collect output events for a while. */
static struct input_event got[2048];
static int ngot;

static void collect(int ms)
{
	ngot = 0;
	long long end = 0;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	end = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000 + ms;
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		long long left = end - (ts.tv_sec * 1000LL + ts.tv_nsec / 1000000);
		if (left <= 0)
			break;
		struct pollfd p = { .fd = virt_fd, .events = POLLIN };
		if (poll(&p, 1, (int)left) <= 0)
			continue;
		struct input_event ev;
		int rc;
		while ((rc = libevdev_next_event(virt, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >= 0)
			if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ngot < 2048)
				got[ngot++] = ev;
	}
}

static int has(unsigned type, unsigned code, int value)
{
	for (int i = 0; i < ngot; i++)
		if (got[i].type == type && got[i].code == code && got[i].value == value)
			return i;
	return -1;
}

static int count_type(unsigned type)
{
	int n = 0;
	for (int i = 0; i < ngot; i++)
		if (got[i].type == type && !(type == EV_SYN && got[i].code != SYN_REPORT))
			n++;
	return n;
}

static void touch(int slot, int tid, int x, int y, int tool_key)
{
	w(EV_ABS, ABS_MT_SLOT, slot);
	w(EV_ABS, ABS_MT_TRACKING_ID, tid);
	w(EV_ABS, ABS_MT_POSITION_X, x);
	w(EV_ABS, ABS_MT_POSITION_Y, y);
	w(EV_ABS, ABS_MT_PRESSURE, 50);
	w(EV_ABS, ABS_X, x);
	w(EV_ABS, ABS_Y, y);
	w(EV_KEY, BTN_TOUCH, 1);
	if (tool_key)
		w(EV_KEY, (unsigned)tool_key, 1);
}

static void moveto(int slot, int x, int y)
{
	w(EV_ABS, ABS_MT_SLOT, slot);
	w(EV_ABS, ABS_MT_POSITION_X, x);
	w(EV_ABS, ABS_MT_POSITION_Y, y);
}

static void lift_all(void)
{
	for (int s = 0; s < 5; s++) {
		w(EV_ABS, ABS_MT_SLOT, s);
		w(EV_ABS, ABS_MT_TRACKING_ID, -1);
	}
	w(EV_KEY, BTN_TOOL_FINGER, 0);
	w(EV_KEY, BTN_TOOL_DOUBLETAP, 0);
	w(EV_KEY, BTN_TOUCH, 0);
	syn();
}

int main(int argc, char **argv)
{
	const char *daemon = argc > 1 ? argv[1] : "./omarchy-edgesd";
	make_fake();
	const char *node = libevdev_uinput_get_devnode(fake);
	if (!node) {
		fprintf(stderr, "fake pad has no devnode\n");
		return 2;
	}
	msleep(300); /* let udev apply the rule */
	if (access(node, R_OK) != 0) {
		fprintf(stderr, "cannot read %s: install the udev rule first\n", node);
		return 2;
	}

	char conf[] = "/tmp/omarchy-edges-e2e-XXXXXX";
	int cfd = mkstemp(conf);
	dprintf(cfd, "osd = false\n");
	close(cfd);

	pid_t pid = fork();
	if (pid == 0) {
		execl(daemon, daemon, "-d", node, "-c", conf, "--debug", (char *)NULL);
		_exit(127);
	}

	for (int i = 0; i < 50 && find_virtual() < 0; i++)
		msleep(100);
	unlink(conf);
	if (!virt) {
		fprintf(stderr, "omarchy-edgesd did not create its virtual touchpad\n");
		kill(pid, SIGTERM);
		return 2;
	}
	msleep(300);
	printf("virtual touchpad: %s\n", libevdev_get_name(virt));
	CHECK(libevdev_has_property(virt, INPUT_PROP_BUTTONPAD), "virtual device keeps INPUT_PROP_BUTTONPAD");
	CHECK(libevdev_get_abs_resolution(virt, ABS_MT_POSITION_X) == 12, "virtual device keeps the resolution");
	CHECK(libevdev_get_id_bustype(virt) == BUS_I2C, "virtual device keeps the bus (internal touchpad for libinput)");

	printf("centre touch\n");
	touch(0, 100, 600, 350, BTN_TOOL_FINGER);
	syn();
	moveto(0, 610, 352);
	syn();
	collect(150);
	CHECK(has(EV_ABS, ABS_MT_TRACKING_ID, 100) >= 0, "contact forwarded");
	CHECK(has(EV_ABS, ABS_MT_POSITION_X, 610) >= 0, "motion forwarded");
	CHECK(has(EV_KEY, BTN_TOOL_FINGER, 1) >= 0, "finger count forwarded");
	lift_all();
	collect(150);
	CHECK(has(EV_ABS, ABS_MT_TRACKING_ID, -1) >= 0, "lift forwarded");

	printf("edge touch held still\n");
	touch(0, 101, W - 20, 400, BTN_TOOL_FINGER);
	syn();
	collect(200);
	CHECK(count_type(EV_ABS) == 0 && count_type(EV_KEY) == 0, "nothing reaches the compositor (%d events)", ngot);

	printf("same touch moves inward\n");
	moveto(0, W - 20 - 60, 400);
	syn();
	collect(150);
	CHECK(has(EV_ABS, ABS_MT_TRACKING_ID, 101) >= 0, "handed back as pointer motion");
	CHECK(has(EV_KEY, BTN_TOUCH, 1) >= 0, "touch begins on the virtual device");
	lift_all();
	collect(100);

	printf("click on the edge\n");
	touch(0, 102, W - 20, 650, BTN_TOOL_FINGER);
	syn();
	collect(100);
	CHECK(ngot == 0, "held back before the click");
	w(EV_KEY, BTN_LEFT, 1);
	syn();
	collect(150);
	int t = has(EV_ABS, ABS_MT_TRACKING_ID, 102), b = has(EV_KEY, BTN_LEFT, 1);
	CHECK(t >= 0 && b >= 0 && t < b, "finger goes out before the button");
	w(EV_KEY, BTN_LEFT, 0);
	syn();
	lift_all();
	collect(100);
	CHECK(has(EV_KEY, BTN_LEFT, 0) >= 0, "button released");

	printf("shutdown\n");
	touch(0, 103, 600, 350, BTN_TOOL_FINGER);
	syn();
	collect(100);
	kill(pid, SIGTERM);
	collect(800);
	CHECK(has(EV_ABS, ABS_MT_TRACKING_ID, -1) >= 0 || has(EV_KEY, BTN_TOUCH, 0) >= 0,
	      "contacts released on SIGTERM");
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "daemon exited cleanly (status %d)", status);
	lift_all();

	libevdev_free(virt);
	close(virt_fd);
	libevdev_uinput_destroy(fake);
	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
