# Weave Stage 0 - Native Observer
# Targets macOS (uses <libproc.h>, Mach absolute time). Will NOT build on
# Linux as-is - see observer-os for the OS-level experiment, which is
# platform-specific by design (that is part of the experiment).

CC ?= cc
CFLAGS = -Wall -Wextra -O2 -std=c11

all: native_observer

native_observer: native_observer.c
	$(CC) $(CFLAGS) -o native_observer native_observer.c

clean:
	rm -f native_observer

.PHONY: all clean
