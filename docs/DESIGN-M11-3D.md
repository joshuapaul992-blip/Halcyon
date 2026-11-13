# M11a: Software 3D

Halcyon's own 3D library for Halcyon programs: a subset of OpenGL 1.3 (the
fixed-function pipeline) drawn by a software renderer written for speed. It
is not a Mesa port; Mesa comes later as the Linux distribution's libraries on
the Linux layer (M13 / M15). Vulkan is a later milestone (M11b, not started).

## Status

Done. The library (`userland/lib/gl`, built as `build/user/libgl.a`), the
float math it uses (`userland/lib/math.c`, `libm.a`), the host tests with
reference pictures, a benchmark (`GLBENCH.EXE`, also on the host), two demos
(the Lantern Valley screen saver, `LANTERNS.SCR`, and the game Shards,
`SHARDS.EXE`, in the Start menu and on the desktop), two QEMU boot variants
and a help page (`help/3d.htm`). No kernel change and no new system call
was needed (see Kernel below).

## Using it

```c
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/hgl.h"

int win = win_create("Demo", 640, 480, WIN_RESIZABLE);
HGLRC rc = hglCreateContext(win, 0);        /* draws into the window's canvas */
hglMakeCurrent(rc);
for (;;) {
    /* events; on EVT_RESIZE: hglResize(rc), then glViewport(...) */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* ... draw ... */
    hglSwapBuffers(rc);                     /* finish the frame and show it */
}
```

A program links `libgl.a` before `libm.a` (see the Makefile's rules for
`SHARDS.EXE`, `LANTERNS.SCR` and `GLBENCH.EXE`). `hglFinishFrame` finishes a
frame without telling the window (screen savers, whose frame `saver.h`
shows; programs that draw text over the 3D picture first);
`hglCreateBufferContext` draws into any pixels (a saver's canvas),
`hglCreateMemoryContext` into memory of its own (tests, benchmarks).
`hglFrameCount` and `hglFramesPerSecond` give the frame counters.

## The API subset

Standard names and enum values (OpenGL 1.3 and the ARB names of
multitexture), so typical OpenGL 1.x code builds with little change.

Supported:

- **State:** `glEnable` / `glDisable` / `glIsEnabled`, every `glGet*` of the
  state below (and the implementation limits), `glGetError`, `glGetString`
  (version "1.3 Halcyon"), `glHint` (accepted; drawing is always
  perspective-correct), `glPushAttrib` / `glPopAttrib` by group (16 deep),
  `glPushClientAttrib` / `glPopClientAttrib`, `glFinish`, `glFlush`.
- **Matrices:** modelview (32 deep), projection (4), texture (4 per unit);
  load, multiply (also transposed), push, pop, rotate, translate, scale,
  frustum, ortho.
- **Vertices:** immediate mode (`glBegin` / `glEnd` with all ten primitive
  types, `glVertex*`, `glColor*`, `glNormal*`, `glTexCoord*`,
  `glMultiTexCoord*`, `glEdgeFlag`, `glRect*`); vertex arrays (`gl*Pointer`
  in byte, short, int, float and double, `glInterleavedArrays` with all 14
  formats, `glArrayElement`, `glDrawArrays`, `glDrawElements`,
  `glDrawRangeElements`, `glClientActiveTexture`); display lists (compile,
  compile and execute, nesting to 64, `glCallLists` with a base and every
  index type).
- **Lighting:** 8 lights (directional, point with constant, linear and
  quadratic attenuation, spot with cut-off and exponent), front and back
  materials, colour material, two-sided lighting, local viewer, the light
  model ambient, separate specular colour, `GL_NORMALIZE`,
  `GL_RESCALE_NORMAL`.
- **Textures:** 2D texture objects, 2 units, powers of two up to 2048,
  every mipmap level; `glTexImage2D` / `glTexSubImage2D` from RGB, RGBA,
  BGR, BGRA, luminance, luminance-alpha, alpha, red, green and blue data in
  unsigned bytes or floats, with the unpacking parameters; internal formats
  RGB, RGBA, luminance, luminance-alpha, alpha, intensity (sized ones are
  stored as 8 bits per channel); filters `GL_NEAREST`, `GL_LINEAR` and the
  four mipmap filters (trilinear included), with GL's magnification
  switch-over; wrap `GL_REPEAT`, `GL_CLAMP_TO_EDGE` (`GL_CLAMP` behaves like
  it: no border colour); environments `GL_MODULATE`, `GL_REPLACE`,
  `GL_DECAL`, `GL_BLEND`, `GL_ADD`; texgen (object linear, eye linear,
  sphere map); texture matrices; `GL_TEXTURE_MAX_LEVEL`.
- **Rasterization:** smooth and flat shading, culling (front, back, both;
  either winding), polygon modes (fill, line, point), polygon offset, wide
  lines and big points (squares), clipping at all six frustum planes (the
  near plane included) and six user planes.
- **Per pixel:** a 32-bit float depth buffer (all eight functions, the
  mask, `glDepthRange`), alpha test, blending (every factor of GL 1.3's
  `glBlendFunc`), fog (linear, exp, exp2), colour mask, scissor test,
  viewports, `glClear` of colour and depth (scissored and masked),
  `glReadPixels` (colour and depth, with the packing parameters).
- **GLU** (`GL/glu.h`): `gluPerspective`, `gluLookAt`, `gluOrtho2D`,
  `gluPickMatrix`, `gluProject`, `gluUnProject`, `gluScaleImage`,
  `gluBuild2DMipmaps` (any size: scaled to the nearest power of two as GLU
  does), `gluErrorString`, `gluGetString`, quadrics (sphere, cylinder, disk
  with normals, texture coordinates, orientation and draw style).

Not supported (calls that are not in `GL/gl.h`, or accepted and ignored as
noted):

- the stencil and accumulation buffers, destination alpha (the colour
  buffer is the window's 0x00RRGGBB canvas: destination-alpha factors act
  as 1), logic ops, dithering (accepted), multisampling;
- 1D, 3D and cube map textures, compressed textures, texture borders and
  border colours, `GL_TEXTURE_MIN_LOD` / `MAX_LOD` / `BASE_LEVEL`
  (accepted), `GL_COMBINE` and `GL_DOT3` environments, `glGetTexImage`,
  `glCopyTexImage2D`;
- `glDrawPixels`, `glBitmap`, `glRasterPos`, `glCopyPixels`, pixel
  transfer, maps and zoom, the imaging subset;
- point, line and polygon smoothing and stippling (the enables are
  accepted, drawing stays aliased and solid);
- selection and feedback (`glRenderMode`), evaluators, colour index mode;
- `glMaterial` between `glBegin` and `glEnd` applies to the whole primitive
  (the vertices are lit when it ends); `GL_DECAL` with luminance and
  intensity textures (undefined in GL) shows the texture.
- Display lists do not record texture images (`glTexImage2D` runs at once;
  texture objects keep images instead), pixel storage, `glReadPixels`,
  `glGet*` or the client state calls (vertex arrays are recorded as the
  vertices they give, as GL says).

## Renderer design

The pipeline: raw vertices (immediate mode or arrays) are processed in
batches (`gl_process`) into clip, eye and window coordinates with colours,
texture coordinates and the fog factor (per-vertex lighting in eye space,
texgen, texture matrices); primitives are assembled, clipped
(Sutherland-Hodgman in homogeneous coordinates, interpolating from the
inside vertex so shared edges clip alike), culled and set up: vertices
snapped to 1/16 pixel, three integer edge functions with the top-left fill
rule, and a plane (value and x / y gradients) for each attribute, the
colours and texture coordinates divided by w so they are interpolated
perspective-correctly, the depth linearly in screen space. Lines become
parallelograms of their width, points squares.

Each triangle is queued with a pointer to a snapshot of the per-pixel state
(taken again only when that state changes) and binned into the 64 x 64
tiles its bounding box touches; clears are queued the same way. When the
frame is finished, the queue fills (8192 records or 512 snapshots), a
texture changes or pixels are read, each tile is drawn on its own with all
its records in order, so the tile's colour and depth stay in the cache.
Tiles share nothing, which leaves the door open for drawing them on several
cores once SMP (M20) exists.

In a tile, a triangle's edges are classified at the tile's corners (64-bit),
then per 8 x 8 block (32-bit): blocks outside are skipped, blocks inside
need no per-pixel edge test. Pixels then go four at a time through SSE2:
depth test (float), 1/w with one division per four pixels, colours,
texturing (one level of detail per four pixels from the texture
coordinates' derivatives; nearest, bilinear, two levels for trilinear;
repeat or clamp) for two units with the environment functions in 16-bit
lanes, the specular sum, fog, alpha test, blending, the colour mask, then
the writes. Rows that end before a group of four are drawn through a copy,
so nothing past the frame buffer's right edge is touched.

The frame buffer is the window's canvas (M9's mapping, `win_canvas`):
frames are drawn straight into it and `hglSwapBuffers` tells the window
manager only the rectangle the frame touched (`win_invalidate_rect`), with
no copy per frame. One buffer: a frame is drawn tile by tile when it is
finished, so a compositor pass in the middle of it shows the tiles already
done. The depth buffer belongs to the context (16-byte aligned for the SSE
loads; the heap gives 8). After `EVT_RESIZE`, `hglResize` maps the canvas
again and resizes the depth buffer and the tile bins.

Deliberate choices for determinism between CPUs: divisions instead of
`rcpps`, and the same `libm` on the host and on Halcyon, so the host tests
see the pictures Halcyon draws.

The float math (`libm.a`, `halmath.h`, included by `hal.h`): `sqrtf fabsf
floorf ceilf truncf roundf lroundf fmodf copysignf fminf fmaxf ldexpf frexpf
hypotf`, `sqrt fabs floor ceil` (double), and `sinf cosf sincosf tanf asinf
acosf atanf atan2f expf exp2f logf log2f log10f powf cbrtf sinhf coshf
tanhf`, with the C names. They work in double precision inside and round
once: within 1 ulp everywhere tested (`sinf` / `cosf` / `tanf` reduce
large arguments with 96 bits of 2/pi). An archive: a program takes only the
functions it calls.

## Frame rates

The benchmark (`GLBENCH.EXE` on Halcyon, `tests/gl/run.sh bench` on the
host, the same source): a mipmapped, fogged floor, 40 lit textured boxes,
a sphere and translucent quads, about 2,400 triangles submitted a frame,
on one core.

| | 640x480 textured, trilinear, lit, fog, blending | untextured | textured only | 1024x768 textured, lit, fog |
|---|---|---|---|---|
| Host (Xeon at 2.1 GHz, gcc -O2) | 110-120 frames/s | 340-410 | 110-130 | 50 |
| QEMU 8.2 without KVM (TCG), same host | 4.5 | 12.5 | 5.1 | |

The roadmap's target, textured scenes at 640x480 at interactive frame rates
on one core, is met with a wide margin on real processors. In QEMU without
KVM every SSE2 instruction is emulated, about 25 times slower: there the
demos run at 4-10 frames a second (Shards at 640x480) and about 1 frame a
second (Lantern Valley full screen at 1024x768 or 1280x800). With KVM the
guest runs at the host's speed.

## Demos

- **Lantern Valley** (`userland/savers/lanterns.c`, `LANTERNS.SCR`): a
  flight up a winding valley at dusk while paper lanterns rise from the
  river. A height field of value noise that repeats along the valley, lit
  by a low sun and a sky light with colour material, a mipmapped noise
  texture, a translucent river, a sky gradient with stars, lanterns as
  camera-facing quads with a glow texture blended additively, linear fog.
  It draws into the saver's pixels through a buffer context, so Display
  Properties' preview and full screen work alike. It logs its 10th, 20th,
  50th and 100th frames and every 1000th with the frame rate.
- **Shards** (`userland/programs/shards.c`, `SHARDS.EXE`): steer a hover
  sled over a floating platform and collect the glowing shards before the
  time runs out; pillars stand in the way; levels add shards and pillars.
  Procedural tile and stone textures with mipmaps, a lit sled mesh,
  octahedral shards with emission and specular, quadric pillars, additive
  glows, blended shadows with polygon offset, exponential fog, a chase
  camera. F shows the frame rate; text is drawn over the 3D picture with
  the window manager's text calls. `/demo` steers by itself and logs its
  frames (the boot test). In the Start menu and on the desktop.

Both are original: no existing game, characters, levels or art.

## Tests

- **Host** (`tests/gl/run.sh`, run by `tools/ci.sh`; ASan and UBSan): the
  library built from the same sources as `libgl.a` (`GL_HOST`) with the same
  float math.
  - 36 scenes compared with reference pictures (`tests/gl/ref/*.qoi`,
    lossless, 192 KB): depth (functions, mask), smooth and flat shading,
    magnification (nearest, linear), every minification filter on a floor
    with colour-coded mipmap levels, `gluBuild2DMipmaps` of a 48 x 40 image,
    wrap modes, the five environments on five texture formats, perspective
    correction, directional, point and spot lights, two-sided lighting and
    colour material, separate specular, linear / exp / exp2 fog, blending,
    the alpha test (no depth where cut), the near plane (geometry behind the
    eye), user clip planes, culling of both faces and windings, two texture
    units (modulate, add), viewports with scissored drawing and clears,
    lines and points, polygon modes and offset, texgen and the texture
    matrix, colour masks, a lit textured coloured mesh. The tolerance is
    robust to tiny floating-point differences: a pixel counts as different
    when one of its channels is off by more than 24, and a scene passes
    with at most 1 % of its pixels different and a mean channel difference
    under 1.0.
  - The references were made once (`scenetest --write`) and each was
    looked at enlarged and checked against what the scene must show (for
    example: the mipmap bands come in level order with distance, blended
    for the `*_MIPMAP_LINEAR` filters and hard for the others; the alpha
    test's holes show the quad drawn behind afterwards; GL_GREATER passes
    only over the nearer square; the spot's disc ends at its cut-off; the
    first fog posts are untouched by linear and exp2 fog and already grey
    with exp; each texture environment cell matches GL's formula).
  - Exact checks: immediate mode, `glDrawElements`, `glDrawArrays`,
    `glArrayElement`, `glInterleavedArrays` with `glDrawRangeElements` and
    a display list draw the same picture bit for bit; a queue that
    overflows (20,000 triangles, each with a new state snapshot) draws the
    same picture as the same scene drawn in small pieces.
  - API checks: errors, `glGet`, the attribute stacks, lights kept in eye
    coordinates, texture names, `gluBuild2DMipmaps` levels, `gluScaleImage`,
    `gluProject` / `gluUnProject`, `glReadPixels`' orientation, display list
    nesting and `glCallLists`, buffer contexts. Result: 36 scenes, 82 checks
    in all, none failed (and the math test's 56 checks).
  - The benchmark (`tests/gl/run.sh bench`): frames per second, not a pass /
    fail test.
- **QEMU** (`tools/boottest.py`, in the matrix):
  - `game3d`: `SHARDS.EXE /demo` runs after boot; the test waits for its
    log of the 50th frame (up to 240 s: the host's speed varies), then a
    screendump of its window must show the rendered scene (not dark, many
    colours, the platform's tiles, the sled, the sky).
  - `saver3d`: Lantern Valley through the screen saver test hook; the test
    waits for "is showing" and the saver's log of its 20th frame, then a
    screendump must show the dusk sky, the green valley and lit lanterns.
  - Both print the frame rates the programs measured.

## Kernel

No kernel change was needed: user programs already run with SSE enabled
(CR4.OSFXSR / OSXMMEXCPT), and every entry into the kernel (system calls and
interrupts, `kernel/core/entry.asm`) saves and restores the full FXSAVE state
on the thread's own kernel stack, so each thread keeps its XMM registers.
No system call was added. The only kernel file touched is the desktop
(`kernel/gui/desktop.c`), for the Start menu and desktop entries of Shards.

## Later

- Drawing tiles on several cores (the queue and tiles are ready for it)
  once SMP exists (M20).
- Cube maps, `GL_COMBINE` / `GL_DOT3`, texture borders and the stencil
  buffer if a program needs them.
- Vulkan (M11b) and hardware 3D (M15) are separate milestones.
