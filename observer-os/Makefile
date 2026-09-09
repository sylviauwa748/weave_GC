# Weave Stage 0 - OS-level Observer
# macOS only (Mach host_statistics64 / task_for_pid / libproc). Run both
# unprivileged and with `sudo` - the difference IS a Stage 0 result.

CC ?= cc
CFLAGS = -Wall -Wextra -O2 -std=c11

all: os_observer

os_observer: os_observer.c
	$(CC) $(CFLAGS) -o os_observer os_observer.c

clean:
	rm -f os_observer

.PHONY: all clean
