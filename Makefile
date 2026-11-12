# Halcyon OS build
BUILD   ?= build
CC      := gcc
LD      := ld
NASM    := nasm
OBJCOPY := objcopy
PY      := python3

KSRC := $(shell find kernel -name '*.c' | sort)
KASM := $(shell find kernel -name '*.asm' | sort)
KOBJ := $(patsubst %.c,$(BUILD)/%.o,$(KSRC)) $(patsubst %.asm,$(BUILD)/%.asm.o,$(KASM)) \
        $(BUILD)/gen/fonts.o $(BUILD)/gen/icons.o

KCFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -mno-red-zone -mcmodel=small -fno-omit-frame-pointer -fno-strict-aliasing \
           -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -Wno-address-of-packed-member -Ikernel -I$(BUILD)/gen -MMD -MP

S2CFLAGS := -m16 -march=i686 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
            -fno-asynchronous-unwind-tables -mgeneral-regs-only -Wall -Wno-unused-function -Wno-array-bounds -Iboot

EFI_SRC := $(wildcard efi/*.c)
EFI_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
              -fno-stack-protector -O2 -Wall -Iboot -Iefi

all: $(BUILD)/halcyon.img

# ---- BIOS boot ----
$(BUILD)/boot/mbr.bin: boot/mbr.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

$(BUILD)/boot/stage2.bin: boot/stage2_entry.S boot/stage2.c boot/stage2.ld boot/bootinfo.h
	@mkdir -p $(dir $@)
	$(CC) $(S2CFLAGS) -c boot/stage2_entry.S -o $(BUILD)/boot/stage2_entry.o
	$(CC) $(S2CFLAGS) -c boot/stage2.c -o $(BUILD)/boot/stage2.o
	$(LD) -m elf_i386 -z noexecstack --no-warn-rwx-segments -T boot/stage2.ld -nostdlib $(BUILD)/boot/stage2_entry.o $(BUILD)/boot/stage2.o -o $(BUILD)/boot/stage2.elf
	$(OBJCOPY) -O binary $(BUILD)/boot/stage2.elf $@
	@sz=$$(stat -c %s $@); if [ $$sz -gt 32768 ]; then echo "stage2 too big ($$sz bytes)"; exit 1; fi

# ---- UEFI boot ----
$(BUILD)/efi/BOOTX64.EFI: $(EFI_SRC) $(wildcard efi/*.h) boot/bootinfo.h
	@mkdir -p $(dir $@)
	clang $(EFI_CFLAGS) -c $(EFI_SRC) -o $(BUILD)/efi/loader.o
	lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$@ $(BUILD)/efi/loader.o

# ---- generated data (fonts, icons) ----
$(BUILD)/gen/fonts.c $(BUILD)/gen/fonts.h &: tools/mkfont.py
	@mkdir -p $(BUILD)/gen
	$(PY) tools/mkfont.py $(BUILD)/gen

$(BUILD)/gen/icons.c $(BUILD)/gen/icons.h &: tools/mkicons.py
	@mkdir -p $(BUILD)/gen
	$(PY) tools/mkicons.py $(BUILD)/gen

$(BUILD)/gen/%.o: $(BUILD)/gen/%.c
	$(CC) $(KCFLAGS) -c $< -o $@

# the version (1.0.<number of commits>), checked on every build
$(BUILD)/gen/version.h: FORCE
	@mkdir -p $(BUILD)/gen
	@sh tools/mkversion.sh $@

GENH := $(BUILD)/gen/fonts.h $(BUILD)/gen/icons.h $(BUILD)/gen/version.h

# ---- kernel ----
$(BUILD)/kernel/%.o: kernel/%.c | $(GENH)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/%.asm.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/kernel.elf: $(KOBJ) kernel/linker.ld
	$(LD) -m elf_x86_64 -z noexecstack -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld $(KOBJ) -o $@

$(BUILD)/KERNEL.BIN: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# ---- disk images ----
IMG_DEPS := $(BUILD)/boot/mbr.bin $(BUILD)/boot/stage2.bin $(BUILD)/KERNEL.BIN \
            $(BUILD)/rootfs.stamp tools/mkimage.sh
ifneq ($(EFI_SRC),)
IMG_DEPS += $(BUILD)/efi/BOOTX64.EFI
endif

# ---- user programs (userland/): static position-independent ELF64 .EXE ----
UCFLAGS := -std=gnu11 -O2 -g0 -ffreestanding -fno-builtin -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables \
           -Wall -Wextra -Wno-unused-parameter -Iuserland/include
ULDFLAGS := -static-pie -nostdlib -Wl,--no-dynamic-linker -Wl,-z,noexecstack -Wl,--build-id=none
ULIB := $(BUILD)/user/crt0.o $(patsubst userland/lib/%.c,$(BUILD)/user/%.o,$(wildcard userland/lib/*.c))
UPROGS := $(patsubst userland/programs/%.c,$(BUILD)/user/%.exe,$(wildcard userland/programs/*.c))

$(BUILD)/user/crt0.o: userland/lib/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/%.o: userland/lib/%.c userland/include/hal.h kernel/core/sysabi.h kernel/net/sockabi.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/%.exe: userland/programs/%.c $(ULIB) userland/include/hal.h kernel/core/sysabi.h kernel/net/sockabi.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(ULDFLAGS) -o $@ $(ULIB) $<
	strip $@

IMG_DEPS += $(UPROGS)
.SECONDARY: $(ULIB)

# ---- libraries linked only into the programs that use them ----
# BearSSL (userland/third_party/bearssl, unmodified): its own C sources with
# our flags, against the compiler's freestanding headers and the few C library
# headers in bearssl-port/ (no system headers). No OS random source or clock:
# libweb seeds it from getrandom() and gives it the time.
BEARSSL := userland/third_party/bearssl
BEARPORT := userland/third_party/bearssl-port
BEAR_SRC := $(shell find $(BEARSSL)/src -name '*.c' | sort)
BEAR_OBJ := $(patsubst $(BEARSSL)/src/%.c,$(BUILD)/user/bearssl/%.o,$(BEAR_SRC))
BEAR_CFLAGS := -std=gnu99 -O2 -g0 -ffreestanding -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables \
               -nostdinc -isystem $(shell $(CC) -print-file-name=include) -I$(BEARPORT) \
               -I$(BEARSSL)/inc -I$(BEARSSL)/src -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_RDRAND=0 \
               -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -Wall -Wextra
$(BUILD)/user/bearssl/%.o: $(BEARSSL)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BEAR_CFLAGS) -c $< -o $@
$(BUILD)/user/libbearssl.a: $(BEAR_OBJ)
	rm -f $@
	ar rcs $@ $^

# libweb (userland/lib/web): URLs, HTTP/HTTPS, inflate; trust anchors from
# Mozilla's CA bundle, converted by tools/mktrust.py
WEBLIB_SRC := $(wildcard userland/lib/web/*.c)
WEBLIB_OBJ := $(patsubst userland/lib/web/%.c,$(BUILD)/user/web/%.o,$(WEBLIB_SRC)) $(BUILD)/user/web/trust.o
$(BUILD)/user/web/trust.c: tools/mktrust.py userland/third_party/cacert/cacert.pem
	@mkdir -p $(dir $@)
	$(PY) tools/mktrust.py userland/third_party/cacert/cacert.pem $@
$(BUILD)/user/web/trust.o: $(BUILD)/user/web/trust.c
	$(CC) $(BEAR_CFLAGS) -c $< -o $@
$(BUILD)/user/web/%.o: userland/lib/web/%.c $(wildcard userland/lib/web/*.h) userland/include/hal.h kernel/core/sysabi.h kernel/net/sockabi.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -I$(BEARPORT) -I$(BEARSSL)/inc -c $< -o $@
$(BUILD)/user/libweb.a: $(WEBLIB_OBJ)
	rm -f $@
	ar rcs $@ $^
WEBLIBS := $(BUILD)/user/libweb.a $(BUILD)/user/libbearssl.a

# programs that use libweb (explicit rules: they replace the pattern rule above)
BROWSER_SRC := $(wildcard userland/programs/browser/*.c)
$(BUILD)/user/browser.exe: userland/programs/browser.c $(BROWSER_SRC) $(wildcard userland/programs/browser/*.h) \
                           userland/lib/web/web.h $(ULIB) $(WEBLIBS) userland/include/hal.h kernel/core/sysabi.h kernel/net/sockabi.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iuserland/lib/web $(ULDFLAGS) -o $@ $(ULIB) $< $(BROWSER_SRC) $(WEBLIBS)
	strip $@
$(BUILD)/user/wget.exe $(BUILD)/user/webtest.exe: $(BUILD)/user/%.exe: userland/programs/%.c userland/lib/web/web.h $(ULIB) $(WEBLIBS) \
                           userland/include/hal.h kernel/core/sysabi.h kernel/net/sockabi.h $(wildcard userland/programs/webtest_certs.h)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iuserland/lib/web -I$(BEARPORT) -I$(BEARSSL)/inc $(ULDFLAGS) -o $@ $(ULIB) $< $(WEBLIBS)
	strip $@

$(BUILD)/halcyon.img: $(IMG_DEPS)
	BUILD=$(BUILD) sh tools/mkimage.sh

# content hash of rootfs/ (file names may contain spaces)
$(BUILD)/rootfs.stamp: FORCE
	@mkdir -p $(BUILD)
	@find rootfs -type f -exec md5sum {} + 2>/dev/null | sort > $@.tmp; cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp

testdisks: tools/mktestdisks.sh
	BUILD=$(BUILD) sh tools/mktestdisks.sh

clean:
	rm -rf ./build

.PHONY: all clean testdisks FORCE
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
