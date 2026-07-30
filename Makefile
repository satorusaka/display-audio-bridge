CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
DAEMON_CFLAGS = $(shell pkg-config --cflags ddcutil libpulse)
DAEMON_LDLIBS = $(shell pkg-config --libs ddcutil libpulse)

PREFIX ?= $(HOME)/.local

.PHONY: all install check clean

all: ddc-volume-daemon ddc-volume-control

ddc-volume-daemon: ddc-volume-control.c
	$(CC) $(CFLAGS) $(DAEMON_CFLAGS) -o $@ $< $(DAEMON_LDLIBS)

ddc-volume-control: ddc-volume-client.c
	$(CC) $(CFLAGS) -o $@ $<

install: ddc-volume-daemon ddc-volume-control
	install -Dm755 ddc-volume-daemon "$(DESTDIR)$(PREFIX)/bin/ddc-volume-daemon"
	install -Dm755 ddc-volume-control "$(DESTDIR)$(PREFIX)/bin/ddc-volume-control"

check: clean all
	@test -x ddc-volume-daemon
	@test -x ddc-volume-control
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libpulse) \
		-o tests/volume-math tests/volume-math.c \
		$(shell pkg-config --libs libpulse)
	./tests/volume-math

clean:
	rm -f ddc-volume-daemon ddc-volume-control tests/volume-math
