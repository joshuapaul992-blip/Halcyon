# Host tests for the storage stack: kernel/fs/*.c compiled unmodified with the
# host compiler against a small libc shim, run against real FAT and exFAT
# images (exFAT results are cross-checked with exfat_tool.py, an independent
# Python implementation, and fsck.exfat) and ISO 9660 images (isotest.c,
# built with xorriso).
#
#   tests/host/run.sh            (== make -C tests/host test)
#                                includes the "hostile" scenario: corrupted
#                                partition tables / FAT / exFAT volumes and the
#                                minimized fuzzer crash inputs (fuzz_regress/)
#
# Fuzzing (see fuzz_fs.c; seeds come from fuzz_seeds.sh):
#   make -C tests/host fuzz-run FUZZ_TIME=600 FUZZ_JOBS=2
#                                libFuzzer (clang); corpus in $(OUT)/fuzz_corpus,
#                                crashes in $(OUT)/fuzz_crashes
#   make -C tests/host fuzz-regress
#                                replay seeds + fuzz_regress/ with the gcc build
#   build/hosttest/fuzz_replay [-v] FILE|DIR...
#                                replay single inputs (gcc + ASan/UBSan)
#
# Needs: gcc, mkfs.fat, fsck.fat, mtools, exfatprogs (mkfs.exfat, fsck.exfat),
# sfdisk, truncate, dd, gzip, python3, xorriso;
# clang with the libFuzzer runtime (libclang-rt-18-dev) for fuzz / fuzz-run.
ROOT    := ../..
OUT     ?= $(ROOT)/build/hosttest
KSRCS   := $(ROOT)/kernel/fs/block.c $(ROOT)/kernel/fs/vfs.c $(ROOT)/kernel/fs/fat.c $(ROOT)/kernel/fs/exfat.c $(ROOT)/kernel/fs/iso9660.c $(ROOT)/kernel/core/unicode.c shim.c
SRCS    := $(KSRCS) fstest.c fuzz_fs.c
HDRS    := $(wildcard $(ROOT)/kernel/fs/*.h) $(wildcard $(ROOT)/kernel/core/*.h) fuzz_fs.h
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -Wno-address-of-packed-member -Wno-unused-result -fno-strict-aliasing
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O1 -g $(WARN) -Wno-format-truncation $(SAN) -I$(ROOT)/kernel
FUZZ_CFLAGS := -std=gnu11 -O1 -g $(WARN) -Wno-incompatible-library-redeclaration -Wno-unused-function \
           -DFUZZ_LIBFUZZER -fsanitize=fuzzer $(SAN) -I$(ROOT)/kernel

FUZZ_TIME ?= 300
FUZZ_JOBS ?= 2
FUZZ_ARGS ?=
CORPUS  := $(OUT)/fuzz_corpus
SEEDS   := $(OUT)/fuzz_seeds

all: $(OUT)/fstest $(OUT)/fuzz_replay

$(OUT)/fstest: $(SRCS) isotest.c $(HDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(SRCS) -o $@

$(OUT)/fuzz_replay: $(KSRCS) fuzz_fs.c fuzz_main.c $(HDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(KSRCS) fuzz_fs.c fuzz_main.c -o $@

$(OUT)/fuzz_fs: $(KSRCS) fuzz_fs.c $(HDRS) Makefile
	@mkdir -p $(OUT)
	clang $(FUZZ_CFLAGS) $(KSRCS) fuzz_fs.c -o $@

$(SEEDS)/.stamp: fuzz_seeds.sh
	@rm -rf $(SEEDS)
	sh fuzz_seeds.sh $(SEEDS) >/dev/null 2>&1
	@touch $@

seeds: $(SEEDS)/.stamp

$(OUT)/displaytest: displaytest.c $(ROOT)/kernel/drivers/edid.c $(ROOT)/kernel/drivers/edid.h $(ROOT)/boot/modesel.h $(ROOT)/boot/bootinfo.h Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) -I$(ROOT)/boot displaytest.c $(ROOT)/kernel/drivers/edid.c -o $@

test: $(OUT)/fstest $(OUT)/displaytest
	$(OUT)/displaytest
	@rm -rf $(OUT)/work && mkdir -p $(OUT)/work
	$(OUT)/fstest $(OUT)/work $(ARGS)

fuzz-regress: $(OUT)/fuzz_replay seeds
	@rm -rf $(OUT)/regress && mkdir -p $(OUT)/regress
	@for f in fuzz_regress/*.gz; do [ -e "$$f" ] && gzip -dc "$$f" > $(OUT)/regress/$$(basename "$$f" .gz); done; true
	$(OUT)/fuzz_replay $(SEEDS) $(OUT)/regress 2>$(OUT)/fuzz_replay.log || { cat $(OUT)/fuzz_replay.log; exit 1; }

fuzz: $(OUT)/fuzz_fs

# -fork keeps fuzzing after a crash; every crash input is saved
fuzz-run: $(OUT)/fuzz_fs seeds
	@mkdir -p $(CORPUS) $(OUT)/fuzz_crashes
	cd $(OUT)/fuzz_crashes && $(abspath $(OUT))/fuzz_fs $(abspath $(CORPUS)) $(abspath $(SEEDS)) \
	    -max_len=2200000 -timeout=20 -rss_limit_mb=4096 -malloc_limit_mb=128 \
	    -max_total_time=$(FUZZ_TIME) -fork=$(FUZZ_JOBS) -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1 \
	    -print_final_stats=1 $(FUZZ_ARGS)

clean:
	rm -rf $(OUT)

.PHONY: all test seeds fuzz fuzz-run fuzz-regress clean
