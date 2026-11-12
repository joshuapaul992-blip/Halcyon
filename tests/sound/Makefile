# Host tests of the WAV library (userland/lib/wav.c), with ASan and UBSan.
#   make -C tests/sound          build
#   tests/sound/run.sh           build and run (also checks the system sounds)
ROOT := ../..
OUT ?= $(ROOT)/build/hostsound
CC := gcc
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/userland/include $(SAN)

all: $(OUT)/wavtest

$(OUT)/wavtest: wavtest.c $(ROOT)/userland/lib/wav.c $(ROOT)/userland/include/wav.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ wavtest.c $(ROOT)/userland/lib/wav.c -lm

clean:
	rm -rf $(OUT)
.PHONY: all clean
