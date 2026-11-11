# Host tests for the storage stack: kernel/fs/*.c compiled unmodified with the
# host compiler against a small libc shim, run against real FAT images.
#
#   tests/host/run.sh            (== make -C tests/host test)
#
# Needs: gcc, mkfs.fat, fsck.fat, mtools, sfdisk, truncate, dd.
ROOT    := ../..
OUT     ?= $(ROOT)/build/hosttest
SRCS    := $(ROOT)/kernel/fs/block.c $(ROOT)/kernel/fs/vfs.c $(ROOT)/kernel/fs/fat.c shim.c fstest.c
HDRS    := $(wildcard $(ROOT)/kernel/fs/*.h) $(wildcard $(ROOT)/kernel/core/*.h)
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -Wno-address-of-packed-member -Wno-unused-result -Wno-format-truncation -fno-strict-aliasing \
           -fsanitize=address,undefined -fno-sanitize-recover=undefined -I$(ROOT)/kernel

all: $(OUT)/fstest

$(OUT)/fstest: $(SRCS) $(HDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(SRCS) -o $@

test: $(OUT)/fstest
	@rm -rf $(OUT)/work && mkdir -p $(OUT)/work
	$(OUT)/fstest $(OUT)/work $(ARGS)

clean:
	rm -rf $(OUT)

.PHONY: all test clean
