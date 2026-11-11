# Host tests of the web library (userland/lib/web) and BearSSL, built for
# Linux. libweb is compiled with ASan and UBSan; BearSSL without (speed).
#   make -C tests/web            build the tests
#   tests/web/run.sh             build and run everything (local servers)
ROOT := ../..
OUT ?= $(ROOT)/build/hostweb
BEAR := $(ROOT)/userland/third_party/bearssl
WEB := $(ROOT)/userland/lib/web
CC := gcc
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
WCFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -DWEB_HOST -D_GNU_SOURCE -I$(WEB) -I$(BEAR)/inc $(SAN)
BCFLAGS := -std=gnu99 -O2 -I$(BEAR)/inc -I$(BEAR)/src

BEAR_SRC := $(shell find $(BEAR)/src -name '*.c' | sort)
BEAR_OBJ := $(patsubst $(BEAR)/src/%.c,$(OUT)/bearssl/%.o,$(BEAR_SRC))
WEB_SRC := $(wildcard $(WEB)/*.c)
WEB_OBJ := $(patsubst $(WEB)/%.c,$(OUT)/web/%.o,$(WEB_SRC)) $(OUT)/web/trust.o
BROWSER := $(ROOT)/userland/programs/browser
TESTS := $(OUT)/inflatetest $(OUT)/urltest $(OUT)/hostget $(OUT)/tatest

all: $(TESTS)

$(OUT)/bearssl/%.o: $(BEAR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BCFLAGS) -c $< -o $@
$(OUT)/libbearssl.a: $(BEAR_OBJ)
	rm -f $@; ar rcs $@ $^

$(OUT)/trust.c: $(ROOT)/tools/mktrust.py $(ROOT)/userland/third_party/cacert/cacert.pem
	@mkdir -p $(OUT)
	python3 $(ROOT)/tools/mktrust.py $(ROOT)/userland/third_party/cacert/cacert.pem $@
$(OUT)/web/trust.o: $(OUT)/trust.c
	@mkdir -p $(dir $@)
	$(CC) $(BCFLAGS) -c $< -o $@
$(OUT)/web/%.o: $(WEB)/%.c $(wildcard $(WEB)/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(WCFLAGS) -c $< -o $@
$(OUT)/libweb.a: $(WEB_OBJ)
	rm -f $@; ar rcs $@ $^

$(OUT)/%: %.c $(OUT)/libweb.a $(OUT)/libbearssl.a
	$(CC) $(WCFLAGS) -o $@ $< $(OUT)/libweb.a $(OUT)/libbearssl.a -lpthread

clean:
	rm -rf $(OUT)
.PHONY: all clean
