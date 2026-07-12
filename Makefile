# octopus-linux — Genoqs Octopus/Nemo Linux port
#
# Usage:
#   make            # build Octopus target
#   make NEMO=1     # build Nemo target
#   make clean

NEMO ?= 0

CC = gcc
CFLAGS = -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
         -std=gnu89 -Wno-implicit-int -Wno-int-conversion \
         -O2 -g -U_FORTIFY_SOURCE \
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

ifeq ($(NEMO),1)
    CFLAGS += -DNEMO \
        -I firmware/NEMO_OS/_NEMO_global \
        -I firmware/NEMO_OS/_NEMO_Viewer \
        -I firmware/NEMO_OS/_NEMO_exe_keys \
        -I firmware/NEMO_OS/_NEMO_exe_rots \
        -I firmware/NEMO_OS/_NEMO_interrupts
    TARGET = build/nemo
else
    TARGET = build/octopus
endif

LDLIBS = -lasound -lpthread -lm
# Phase 2+ will add: -llo

# Our new source files — main_linux.c is the single TU that includes everything
NEW_SRCS = src/main_linux.c \
           src/hal_linux.c \
           src/midi_alsa.c \
           src/osc_server.c \
           src/osc_render.c \
           src/flash_file.c

# The original .c files are #included directly into main_linux.c (single TU),
# matching the original firmware architecture. Do NOT compile them separately.

SRCS = $(NEW_SRCS)

OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) build/octopus build/nemo

.PHONY: clean
