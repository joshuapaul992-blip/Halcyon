# Host tests of Halcyon's float math (userland/lib/math.c), with ASan and
# UBSan. (The 3D library's scene tests will join them here: M11a, in progress.)
#   make -C tests/gl             build
#   tests/gl/run.sh              build and run
ROOT := ../..
OUT ?= $(ROOT)/build/hostgl
CC := gcc
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-builtin -I$(ROOT)/userland/include $(SAN)

all: $(OUT)/mathtest

$(OUT)/mathtest: mathtest.c $(ROOT)/userland/lib/math.c $(ROOT)/userland/include/halmath.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ mathtest.c -lm

clean:
	rm -rf $(OUT)
.PHONY: all clean
