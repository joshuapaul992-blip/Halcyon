# M11a: Software 3D (in progress)

Halcyon's own 3D library for Halcyon programs: a subset of OpenGL 1.3 (the
fixed-function pipeline) drawn by a software renderer written for speed. It
is not a Mesa port; Mesa comes later as the Linux distribution's libraries on
the Linux layer (M13 / M15). Vulkan is a later milestone (M11b, not started).

## Status

Work stopped at a clean point, at the user's request. Nothing of the 3D
library is built into the image yet, so nothing unfinished can be reached
from the Start menu, the desktop or the screen saver list.

Done (built, tested, in CI):

- **Float math** (`userland/lib/math.c`, `userland/include/halmath.h`,
  included by `hal.h`): `sqrtf fabsf floorf ceilf truncf roundf lroundf
  fmodf copysignf fminf fmaxf ldexpf frexpf hypotf`, `sqrt fabs floor ceil`
  (double), and `sinf cosf sincosf tanf asinf acosf atanf atan2f expf exp2f
  logf log2f log10f powf cbrtf sinhf coshf tanhf`, with the C names. They
  work in double precision inside (range reduction, short series, scaling by
  powers of two built from bits) and round once: within 1 ulp everywhere
  tested, the exact ones bit for bit. `sinf` / `cosf` / `tanf` use a
  three-part pi/2 below 2^20 and a Payne-Hanek reduction with 96 bits of 2/pi
  above, so huge arguments are accurate too. Built as the archive
  `build/user/libm.a`, linked after every program's own objects, so a
  program takes only the functions it calls (programs that call none are
  unchanged).
- **Math test** (`tests/gl/mathtest.c`, `tests/gl/run.sh`, run by
  `tools/ci.sh`): every function against the host's double-precision
  library over random and log-uniform ranges (200,000 values per range, over
  5 million in all), worst error in ulps; floor / ceil / trunc / round / fabs /
  fmod / frexp / ldexp bit for bit against the host on 400,000 values; NaN,
  infinities, signed zeros and domain errors. Result: 56 checks, 0 failed;
  every accuracy range measured 0.000 ulp worst case (correctly rounded on
  all values tried).

Written, not yet built, linked or tested (the sources compile without
warnings on the host and with the userland flags, nothing more):

- `userland/include/GL/gl.h` (the GL 1.3 subset's names and enums),
  `GL/glu.h` and `GL/hgl.h` (the window binding's interface),
- `userland/lib/gl/glint.h` (context, state groups, vertices, the queue),
- `matrix.c`: matrix stacks (modelview 32, projection 4, texture 4 per
  unit), rotate / translate / scale / frustum / ortho, transposed loads,
  user clip planes;
- `vertex.c`: immediate mode, vertex arrays (`glDrawArrays`,
  `glDrawElements`, `glDrawRangeElements`, `glArrayElement`,
  `glInterleavedArrays`), transform, clip codes, lighting (8 lights:
  directional, point, spot; materials, color material, two-sided, local
  viewer, separate specular), texgen (object / eye linear, sphere map),
  texture matrices, per-vertex fog (linear, exp, exp2);
- `prim.c`: primitive assembly (all ten modes), homogeneous clipping against
  the frustum (near plane included) and user planes, culling, polygon modes,
  lines and points as quads, flat shading, polygon offset, and triangle
  set-up (4 sub-pixel bits, integer edge functions with the top-left rule,
  perspective-correct attribute planes);
- `raster.c`: the tile queue (64 x 64 tiles, state snapshots, bins), clears
  as tile commands, the SSE2 pixel pipeline (8 x 8 block classification,
  4 pixels per step: float depth buffer, 1/w with one division per 4
  pixels, nearest / bilinear / mipmapped / trilinear sampling with repeat and
  clamp, GL_MODULATE / REPLACE / DECAL / BLEND / ADD for 2 units, specular
  sum, fog, alpha test, blending, color mask), `glReadPixels`;
- `texture.c`: texture objects, `glTexImage2D` / `glTexSubImage2D` with
  pixel-store unpacking, parameters, environment, texgen state, mipmap
  completeness.

Not done:

- `context.c` (context creation, enable / disable, `glGet*`, push / pop
  attributes, errors, strings), `list.c` (display lists), `glu.c`, `hgl.c`
  (the window binding to the canvas), and the Makefile rule for `libgl.a`;
- the scene tests against reference images, the benchmark, the QEMU boot
  variant;
- the 3D screen saver and the game, their Start menu, desktop and help
  entries (`help/3d.htm`);
- frame rates: none measured yet (nothing renders yet).

## Design (as written so far)

Pipeline: raw vertices (immediate mode or arrays) are processed in batches
(`gl_process`) into clip, eye and window coordinates with colors, texture
coordinates and the fog factor; primitives are assembled, clipped
(Sutherland-Hodgman, interpolating from the inside vertex so shared edges
clip alike), culled and set up; each triangle is queued with a pointer to a
snapshot of the per-pixel state (taken again only when that state changes)
and binned into the tiles its bounding box touches. When the frame is
finished, the queue fills, textures change or pixels are read, each tile is
drawn on its own with all its triangles in order, so the tile's color and
depth stay in the cache; tiles share nothing, which leaves the door open for
drawing tiles on several cores once SMP (M20) exists.

The frame buffer is the window's canvas (M9's mapping, `win_canvas`):
frames are drawn straight into it and shown with `win_invalidate`, with no
copy per frame. The color buffer has no alpha (destination-alpha blend
factors act as 1). The depth buffer is 32-bit float in [0, 1]. Deliberate
choices for determinism between CPUs: divisions instead of `rcpps`, and the
same `libm` on the host and on Halcyon.

## Kernel

No kernel change was needed: user programs already run with SSE enabled
(CR4.OSFXSR / OSXMMEXCPT), and every entry into the kernel (system calls and
interrupts, `kernel/core/entry.asm`) saves and restores the full FXSAVE state
on the thread's own kernel stack, so each thread keeps its XMM registers.
No system call was added.
