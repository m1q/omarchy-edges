PREFIX       ?= $(HOME)/.local
CC           ?= cc
CFLAGS       ?= -O2 -g
EXTRA_CFLAGS ?=
BASE_CFLAGS  := -std=gnu11 -Wall -Wextra -Wshadow -D_GNU_SOURCE
EVDEV_CFLAGS := $(shell pkg-config --cflags libevdev)
EVDEV_LIBS   := $(shell pkg-config --libs libevdev)
ALL_CFLAGS    = $(CFLAGS) $(BASE_CFLAGS) $(EXTRA_CFLAGS)

SRC  = src/main.c src/engine.c src/actions.c src/config.c
HDR  = src/engine.h src/actions.h src/config.h

all: omarchy-edgesd

omarchy-edgesd: $(SRC) $(HDR)
	$(CC) $(ALL_CFLAGS) $(EVDEV_CFLAGS) -o $@ $(SRC) $(EVDEV_LIBS) -lpthread -lm

tests/test_engine: tests/test_engine.c src/engine.c src/config.c $(HDR)
	$(CC) $(ALL_CFLAGS) -fsanitize=address,undefined -o $@ tests/test_engine.c src/engine.c src/config.c -lm

test: tests/test_engine
	./tests/test_engine

tests/e2e: tests/e2e.c
	$(CC) $(ALL_CFLAGS) $(EVDEV_CFLAGS) -o $@ tests/e2e.c $(EVDEV_LIBS)

# Needs the udev rule: creates a fake clickpad and runs the daemon on it.
e2e: omarchy-edgesd tests/e2e
	./tests/e2e ./omarchy-edgesd

install: omarchy-edgesd
	install -Dm755 omarchy-edgesd $(PREFIX)/bin/omarchy-edgesd

clean:
	rm -f omarchy-edgesd tests/test_engine tests/e2e

.PHONY: all test e2e install clean
