CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
DAEMON_CFLAGS = $(shell pkg-config --cflags ddcutil libpulse)
DAEMON_LDLIBS = $(shell pkg-config --libs ddcutil libpulse) -lm

PREFIX ?= $(HOME)/.local

.PHONY: all install check clean

all: display-audio-worker

display-audio-worker: display-audio-daemon.c
	$(CC) $(CFLAGS) $(DAEMON_CFLAGS) -o $@ $< $(DAEMON_LDLIBS)

install: display-audio-worker
	install -Dm755 display-audio-worker "$(DESTDIR)$(PREFIX)/bin/display-audio-worker"
	install -Dm755 display_audio_manager.py "$(DESTDIR)$(PREFIX)/bin/display-audio-daemon"
	install -Dm755 display_audio_cli.py "$(DESTDIR)$(PREFIX)/bin/display-audio"
	install -Dm755 display_audio_settings.py "$(DESTDIR)$(PREFIX)/bin/display-audio-settings"
	install -Dm644 display_audio_common.py \
		"$(DESTDIR)$(PREFIX)/lib/display-audio/display_audio_common.py"
	install -Dm644 display_audio_common.py \
		"$(DESTDIR)$(PREFIX)/bin/display_audio_common.py"

check: clean all
	@test -x display-audio-worker
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libpulse) \
		-o tests/volume-math tests/volume-math.c \
		$(shell pkg-config --libs libpulse) -lm
	python -m unittest discover -s tests -p 'test_*.py'
	./tests/volume-math

clean:
	rm -f display-audio-worker tests/volume-math
