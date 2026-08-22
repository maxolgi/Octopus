# octopus-linux — Genoqs Octopus Linux port
#
# Usage:
#   make              # build standalone engine (build/octopus) + static lib (build/liboctopus.a)
#   make clean

CC = gcc
AR = ar

CFLAGS = -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
         -std=gnu89 -Wno-implicit-int -Wno-int-conversion \
         -O2 -g -U_FORTIFY_SOURCE -fPIC \
        -I include \
        -I firmware/OCT_OS \
        -I firmware/OCT_OS/_OCT_global \
        -I firmware/OCT_OS/_OCT_objects \
        -I firmware/OCT_OS/_OCT_Player \
        -I firmware/OCT_OS/_OCT_Viewer \
        -I firmware/OCT_OS/_OCT_exe_keys \
        -I firmware/OCT_OS/_OCT_exe_rots \
        -I firmware/OCT_OS/_OCT_init \
        -I firmware/OCT_OS/_OCT_interrupts

LDLIBS = -lasound -lpthread -lm

# Engine core — engine.c is the single TU that includes all firmware sources.
# engine_main.c is the standalone entry point (not part of the static lib,
# which is linked into the Rust GUI/CLI binaries that provide their own main).
ENGINE_SRCS = src/engine.c \
              src/hal_linux.c \
              src/midi_alsa.c \
              src/osc_server.c \
              src/osc_render.c \
              src/flash_file.c

ENGINE_OBJS = $(ENGINE_SRCS:.c=.o)

all: build/octopus build/liboctopus.a

build/liboctopus.a: $(ENGINE_OBJS)
	@mkdir -p build
	$(AR) rcs $@ $(ENGINE_OBJS)

build/octopus: src/engine_main.o build/liboctopus.a
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/engine_main.o build/liboctopus.a $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(ENGINE_OBJS) src/engine_main.o build/octopus build/liboctopus.a

.PHONY: clean
