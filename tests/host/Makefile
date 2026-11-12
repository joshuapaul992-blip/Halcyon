# Host tests for the storage stack: kernel/fs/*.c compiled unmodified with the
# host compiler against a small libc shim, run against real FAT and exFAT
# images (exFAT results are cross-checked with exfat_tool.py, an independent
# Python implementation, and fsck.exfat) and ISO 9660 images (isotest.c,
# built with xorriso). Also the keyboard layouts (kbdtest.c: kernel/drivers/
# keymap.c and keylayouts.c), the USB HID report descriptor parser and report
# decoder (kernel/usb/hidparse.c, hidtest.c), and the EDID parser and the
# loaders' video mode choice (displaytest.c), the display timings and mode
# lists (modetest.c: kernel/drivers/modes.c), the monitor arrangement math
# (layouttest.c: kernel/gui/layout.c), and the HD Audio codec layer
# (kernel/sound/drivers/hda_codec.c against synthetic codecs, hdatest.c),
# and the NTFS formatter (kernel/fs/ntfs_mkfs.c, ntfsfmt.c): volumes of many
# sizes, sector and cluster sizes checked with ntfsfix, ntfsinfo, the
# independent reader ntfs_tool.py and the ntfs-3g FUSE driver
# (ntfs_fuse.py), and compared with mkntfs's layout.
#
#   tests/host/run.sh            (== make -C tests/host kbd test)
#   make -C tests/host kbd       keyboard layouts only
#   make -C tests/host hid       only the HID tests (HIDARGS=-v lists every report)
#   make -C tests/host hda       only the HD Audio codec tests (HDAARGS=-v dumps the codecs)
#   make -C tests/host ntfsfmt   only the NTFS formatter tests (NTFSARGS="-v quick")
#   make -C tests/host notepad   only Notepad's text model and undo (NPARGS=-v)
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
# Network stack (netsim.c, net_shim.c; see netsim.c): kernel/net/ built as a
# shared object, one copy per simulated computer, on a simulated Ethernet:
#   make -C tests/host net       the simulator's tests (NETARGS="-v tcp" ...)
#   make -C tests/host fuzz-net-run FUZZ_TIME=600
#                                libFuzzer on the receive path (fuzz_net.c);
#                                seeds are frames the simulator sent
#   make -C tests/host fuzz-net-regress
#
# Needs: gcc, mkfs.fat, fsck.fat, mtools, exfatprogs (mkfs.exfat, fsck.exfat),
# sfdisk, truncate, dd, gzip, python3, xorriso, ntfs-3g (ntfsfix, ntfsinfo,
# ntfsresize, mkntfs, ntfsls, ntfscat, the ntfs-3g FUSE driver and
# fusermount, used as root and with /dev/fuse: the NTFS formatter tests and
# the NTFS driver scenarios ntfs, ntfsw, ntfssafe, ntfsbig of fstest
# (ntfstest.c) are skipped with a message without them);
# clang with the libFuzzer runtime (libclang-rt-18-dev) for fuzz / fuzz-run.
ROOT    := ../..
OUT     ?= $(ROOT)/build/hosttest
NTFS_SRCS := $(addprefix $(ROOT)/kernel/fs/,ntfs.c ntfs_mft.c ntfs_index.c ntfs_alloc.c ntfs_lznt1.c)
KSRCS   := $(ROOT)/kernel/fs/block.c $(ROOT)/kernel/fs/vfs.c $(ROOT)/kernel/fs/fat.c $(ROOT)/kernel/fs/exfat.c $(ROOT)/kernel/fs/iso9660.c \
           $(NTFS_SRCS) $(ROOT)/kernel/core/unicode.c shim.c
SRCS    := $(KSRCS) $(ROOT)/kernel/fs/ntfs_mkfs.c $(ROOT)/kernel/fs/ntfs_upcase.c $(ROOT)/kernel/apps/recycle.c fstest.c fuzz_fs.c
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

KBD_SRCS := kbdtest.c $(ROOT)/kernel/drivers/keymap.c $(ROOT)/kernel/drivers/keylayouts.c $(ROOT)/kernel/core/unicode.c

all: $(OUT)/fstest $(OUT)/fuzz_replay $(OUT)/kbdtest $(OUT)/hidtest $(OUT)/displaytest $(OUT)/netsim $(OUT)/netstack.so \
     $(OUT)/hdatest $(OUT)/ntfsfmt $(OUT)/notepadtest

NETSRCS := $(addprefix $(ROOT)/kernel/net/,socket.c local.c inet.c eth.c ip.c udp.c tcp.c dhcp.c dns.c)
NETHDRS := $(wildcard $(ROOT)/kernel/net/*.h) $(wildcard $(ROOT)/kernel/core/*.h) netsim.h

$(OUT)/netstack.so: $(NETSRCS) net_shim.c $(NETHDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) -fPIC -shared -fvisibility=hidden -Wl,-Bsymbolic $(NETSRCS) net_shim.c -o $@

$(OUT)/netsim: netsim.c netsim.h $(ROOT)/kernel/net/sockabi.h Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) netsim.c -o $@ -ldl

# (the cached socket buffers of every unloaded copy would count as leaks:
# netsim checks each stack's memory itself)
net: $(OUT)/netsim $(OUT)/netstack.so
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/netsim $(OUT)/netstack.so $(NETARGS)

FUZZNET_SRCS := $(NETSRCS) net_shim.c fuzz_net.c
$(OUT)/fuzz_net: $(FUZZNET_SRCS) $(NETHDRS) Makefile
	@mkdir -p $(OUT)
	clang $(FUZZ_CFLAGS) $(FUZZNET_SRCS) -o $@

$(OUT)/fuzz_net_replay: $(FUZZNET_SRCS) $(NETHDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(FUZZNET_SRCS) -o $@

NETSEEDS := $(OUT)/fuzz_net_seeds
$(NETSEEDS)/.stamp: $(OUT)/netsim $(OUT)/netstack.so
	@rm -rf $(NETSEEDS)
	ASAN_OPTIONS=detect_leaks=0 $(OUT)/netsim $(OUT)/netstack.so -seeds $(NETSEEDS) arp dhcp dns udp tcp rst >/dev/null
	@touch $@

fuzz-net-run: $(OUT)/fuzz_net $(NETSEEDS)/.stamp
	@mkdir -p $(OUT)/fuzz_net_corpus $(OUT)/fuzz_net_crashes
	cd $(OUT)/fuzz_net_crashes && $(abspath $(OUT))/fuzz_net $(abspath $(OUT))/fuzz_net_corpus $(abspath $(NETSEEDS)) \
	    -max_len=20000 -timeout=20 -rss_limit_mb=2048 -max_total_time=$(FUZZ_TIME) -fork=$(FUZZ_JOBS) \
	    -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1 -print_final_stats=1 $(FUZZ_ARGS)

fuzz-net-regress: $(OUT)/fuzz_net_replay $(NETSEEDS)/.stamp
	$(OUT)/fuzz_net_replay $(NETSEEDS) $(wildcard fuzz_net_regress)

$(OUT)/kbdtest: $(KBD_SRCS) $(ROOT)/kernel/drivers/keymap.h $(ROOT)/kernel/drivers/input.h Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(KBD_SRCS) -o $@

kbd: $(OUT)/kbdtest
	$(OUT)/kbdtest

# SETTINGS.INI (kernel/core/settings.c): settingstest.c on an in-memory VFS
# with an error or a power cut at every step; settingsfs.c on real FAT32,
# exFAT and NTFS images with a power cut at every sector written
$(OUT)/settingstest: settingstest.c $(ROOT)/kernel/core/settings.c $(ROOT)/kernel/core/settings.h Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) settingstest.c $(ROOT)/kernel/core/settings.c -o $@
SETFS_SRCS := $(filter-out shim.c,$(KSRCS)) $(ROOT)/kernel/core/settings.c settingsfs.c
$(OUT)/settingsfs: $(SETFS_SRCS) shim.c $(HDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) -DSHIM_NO_SETTINGS $(SETFS_SRCS) shim.c -o $@
settings: $(OUT)/settingstest $(OUT)/settingsfs
	$(OUT)/settingstest $(SETARGS)
	@rm -rf $(OUT)/setwork && mkdir -p $(OUT)/setwork
	$(OUT)/settingsfs $(OUT)/setwork $(SETARGS)

$(OUT)/fstest: $(SRCS) isotest.c ntfstest.c recycletest.c $(ROOT)/kernel/apps/recycle.h $(HDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(SRCS) -o $@

# the NTFS formatter (ntfsfmt.c): images checked with ntfs-3g's tools,
# ntfs_tool.py and the ntfs-3g FUSE driver (ntfs_fuse.py)
NTFSFMT_SRCS := $(KSRCS) $(ROOT)/kernel/fs/ntfs_mkfs.c $(ROOT)/kernel/fs/ntfs_upcase.c ntfsfmt.c

$(OUT)/ntfsfmt: $(NTFSFMT_SRCS) $(HDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(NTFSFMT_SRCS) -o $@

ntfsfmt: $(OUT)/ntfsfmt
	@rm -rf $(OUT)/ntfswork && mkdir -p $(OUT)/ntfswork
	$(OUT)/ntfsfmt $(OUT)/ntfswork $(NTFSARGS)

# the installer's partition tables (kernel/fs/parttab.c, parttest.c): checked
# with gpt_check.py (independent, zlib CRCs), sfdisk and sgdisk (if installed)
$(OUT)/parttest: $(ROOT)/kernel/fs/parttab.c $(ROOT)/kernel/fs/parttab.h parttest.c shim.c $(wildcard $(ROOT)/kernel/core/*.h) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(ROOT)/kernel/fs/parttab.c parttest.c shim.c -o $@

parttab: $(OUT)/parttest
	@rm -rf $(OUT)/partwork && mkdir -p $(OUT)/partwork
	$(OUT)/parttest $(OUT)/partwork $(PARTARGS)

# Notepad's text model and undo engine (userland/programs/notepad/doc.c,
# undo.c), compiled for the host (-DNP_HOST) with the user library's UTF-8
NPDIR := $(ROOT)/userland/programs/notepad
NPSRCS := notepadtest.c $(NPDIR)/doc.c $(NPDIR)/undo.c $(ROOT)/userland/lib/utf8.c
$(OUT)/notepadtest: $(NPSRCS) $(NPDIR)/doc.h $(NPDIR)/undo.h Makefile
	@mkdir -p $(OUT)
	gcc -std=gnu11 -O1 -g $(WARN) $(SAN) -DNP_HOST -I$(NPDIR) $(NPSRCS) -o $@

notepad: $(OUT)/notepadtest
	$(OUT)/notepadtest $(NPARGS)

HIDSRCS := $(ROOT)/kernel/usb/hidparse.c hidtest.c
HIDHDRS := $(ROOT)/kernel/usb/hidparse.h $(ROOT)/kernel/drivers/input.h $(wildcard $(ROOT)/kernel/core/*.h)

$(OUT)/hidtest: $(HIDSRCS) $(HIDHDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(HIDSRCS) -o $@

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

$(OUT)/modetest: modetest.c $(ROOT)/kernel/drivers/modes.c $(ROOT)/kernel/drivers/modes.h Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) modetest.c $(ROOT)/kernel/drivers/modes.c -o $@

$(OUT)/layouttest: layouttest.c $(ROOT)/kernel/gui/layout.c $(ROOT)/kernel/gui/layout.h $(ROOT)/kernel/gui/rect.h Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) layouttest.c $(ROOT)/kernel/gui/layout.c -o $@

display: $(OUT)/displaytest $(OUT)/modetest $(OUT)/layouttest
	$(OUT)/displaytest
	$(OUT)/modetest
	$(OUT)/layouttest

hid: $(OUT)/hidtest
	$(OUT)/hidtest $(HIDARGS)

# NVIDIA identification (kernel/drivers/nv/: chip table, VBIOS / DCB parser,
# I2C and AUX engines; nvtest.c generates the images and simulates the buses)
# and its fuzzer (fuzz_nv.c; seeds: every generated image layout)
NVSRCS := $(addprefix $(ROOT)/kernel/drivers/nv/,chip.c vbios.c i2c.c) $(ROOT)/kernel/drivers/edid.c
NVHDRS := $(ROOT)/kernel/drivers/nv/nvhw.h $(ROOT)/kernel/drivers/edid.h $(wildcard $(ROOT)/kernel/core/*.h)
$(OUT)/nvtest: nvtest.c $(NVSRCS) $(NVHDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) nvtest.c $(NVSRCS) -o $@

nv: $(OUT)/nvtest
	$(OUT)/nvtest $(NVARGS)

# M10b: the EVO display engine (evo.c, evodp.c, evopll.c, evoscript.c)
# against a simulated display engine and DisplayPort sink (evotest.c)
EVOSRCS := $(addprefix $(ROOT)/kernel/drivers/nv/,evo.c evodp.c evopll.c evoscript.c vbios.c chip.c)
EVOHDRS := $(ROOT)/kernel/drivers/nv/evo.h $(NVHDRS)
$(OUT)/evotest: evotest.c $(EVOSRCS) $(EVOHDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) evotest.c $(EVOSRCS) -o $@

evo: $(OUT)/evotest
	$(OUT)/evotest $(EVOARGS)

$(OUT)/fuzz_nv: fuzz_nv.c $(NVSRCS) $(NVHDRS) Makefile
	@mkdir -p $(OUT)
	clang $(FUZZ_CFLAGS) fuzz_nv.c $(NVSRCS) -o $@

$(OUT)/fuzz_nv_replay: fuzz_nv.c $(NVSRCS) $(NVHDRS) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) fuzz_nv.c $(NVSRCS) -o $@

NVSEEDS := $(OUT)/fuzz_nv_seeds
$(NVSEEDS)/.stamp: $(OUT)/nvtest
	@rm -rf $(NVSEEDS) && mkdir -p $(NVSEEDS)
	$(OUT)/nvtest --seeds $(NVSEEDS)
	@touch $@

fuzz-nv-run: $(OUT)/fuzz_nv $(NVSEEDS)/.stamp
	@mkdir -p $(OUT)/fuzz_nv_corpus $(OUT)/fuzz_nv_crashes
	cd $(OUT)/fuzz_nv_crashes && $(abspath $(OUT))/fuzz_nv $(abspath $(OUT))/fuzz_nv_corpus $(abspath $(NVSEEDS)) \
	    -max_len=70000 -timeout=20 -rss_limit_mb=2048 -max_total_time=$(FUZZ_TIME) -fork=$(FUZZ_JOBS) \
	    -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1 -print_final_stats=1 $(FUZZ_ARGS)

fuzz-nv-regress: $(OUT)/fuzz_nv_replay $(NVSEEDS)/.stamp
	$(OUT)/fuzz_nv_replay $(NVSEEDS) $(wildcard fuzz_nv_regress)

HDASRCS := $(ROOT)/kernel/sound/drivers/hda_codec.c hdatest.c
$(OUT)/hdatest: $(HDASRCS) $(ROOT)/kernel/sound/drivers/hda.h $(wildcard $(ROOT)/kernel/core/*.h) Makefile
	@mkdir -p $(OUT)
	gcc $(CFLAGS) $(HDASRCS) -o $@

hda: $(OUT)/hdatest
	$(OUT)/hdatest $(HDAARGS)

test: $(OUT)/fstest display hid hda
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

.PHONY: all kbd hid hda ntfsfmt notepad test seeds fuzz fuzz-run fuzz-regress net fuzz-net-run fuzz-net-regress clean
