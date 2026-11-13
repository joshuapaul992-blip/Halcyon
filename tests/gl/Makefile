# Host tests of Halcyon's float math (userland/lib/math.c) and 3D library
# (userland/lib/gl): the same sources as on Halcyon, built for Linux with
# ASan and UBSan (GL_HOST); the benchmark without them, optimized as on
# Halcyon.
#   make -C tests/gl             build
#   tests/gl/run.sh              build and run (math, scenes)
#   tests/gl/run.sh bench        the benchmark (frames per second)
ROOT := ../..
OUT ?= $(ROOT)/build/hostgl
CC := gcc
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-builtin -I$(ROOT)/userland/include $(SAN)
GL := $(ROOT)/userland/lib/gl
GL_SRC := $(filter-out $(GL)/hgl.c,$(wildcard $(GL)/*.c))
GL_DEPS := $(GL_SRC) $(wildcard $(GL)/*.h) $(wildcard $(ROOT)/userland/include/GL/*.h) $(ROOT)/userland/lib/math.c $(ROOT)/userland/include/halmath.h
GLFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fno-builtin -DGL_HOST -I$(ROOT)/userland/include

all: $(OUT)/mathtest $(OUT)/scenetest $(OUT)/bench

$(OUT)/mathtest: mathtest.c $(ROOT)/userland/lib/math.c $(ROOT)/userland/include/halmath.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ mathtest.c -lm

# (no -lm: the library's own float math, as on Halcyon)
$(OUT)/scenetest: scenetest.c scenes.h qoi.h $(GL_DEPS)
	@mkdir -p $(OUT)
	$(CC) $(GLFLAGS) -g -O1 $(SAN) -o $@ scenetest.c $(GL_SRC) $(ROOT)/userland/lib/math.c

# (the same benchmark as GLBENCH.EXE on Halcyon)
$(OUT)/bench: $(ROOT)/userland/programs/glbench.c $(GL_DEPS)
	@mkdir -p $(OUT)
	$(CC) $(GLFLAGS) -O2 -o $@ $(ROOT)/userland/programs/glbench.c $(GL_SRC) $(ROOT)/userland/lib/math.c

clean:
	rm -rf $(OUT)
.PHONY: all clean
