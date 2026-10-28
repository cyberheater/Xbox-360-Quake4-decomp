# q4hle — native (HLE) renderer for the Quake 4 ReXGlue port

`q4hle.exe` is a second executable built from the same recompiled code as `q4.exe`. It replaces
Xenia's Xenos GPU emulation with a renderer written for this game. It follows the approach of
EdgeOfTimeRecomp: hook the game's Direct3D calls and replay them on the PC's GPU. The renderer
here is written from scratch for Direct3D 11.

`q4.exe` is unchanged. Build `q4hle` with:

```
cmake --preset win-amd64-release -DQ4_HLE=ON
cmake --build --preset win-amd64-release --target q4hle
```

## How it works

```
 game code (recompiled PPC)
   │  D3DDevice_* calls  (Xbox 360 D3D library, 0x825DE000-0x82605000)
   ▼
 trace_hooks.cpp   wrappers around all 658 D3D library functions (trace, watchdog,
   │               and per-function native handlers)
   ├── the original guest function runs first: it updates the guest device's
   │   register shadows and writes PM4 into the ring buffer
   ▼
 renderer.cpp      reads the GPU state from the device shadow and draws with D3D11
   ├── xenos_hlsl.cpp  Xenos microcode -> HLSL (SM 5.0), compiled with D3DCompile, cached on disk
   ├── textures.cpp    texture fetch constants -> D3D11 textures (tiling, endianness, DXT, mips)
   └── native.cpp      D3D11 device and swap chain on the game window

 null_gpu.cpp      stand-in for the GPU: consumes the ring, executes the synchronisation packets
                   (fences, scratch writes, waits, interrupts) and ticks vblank. Nothing is rasterised.
```

### Guest D3D device layout (2005 XDK, recovered from the decompile)

| Offset | Contents |
|---|---|
| `+0x000` | PM4 write pointer (`+0x008` limit) |
| `+0x010..0x030` | dirty masks (VS consts, PS consts, fetch, register groups A/B) |
| `+0x480` | fetch constants (32 × 6 dwords, register 0x4800) |
| `+0x780` / `+0x1780` | VS / PS float constants (256 × float4 each) |
| `+0x2780` | bool and loop constants (register 0x4900) |
| `+0x2CC0 …` | register shadows, by group: 0x2000@2CC0, 0x2100@2D0C, 0x2180@2D60, 0x2200@2D74, 0x2280@2DA4, 0x2300@2DF8, 0x2380@2E90 |
| `+0x2890` | pointer to the GPU write-back block (fence at +0, ring read pointer at +0x3C) |
| `+0x2894` | interrupt/swap block (scratch registers write back here) |
| `+0x289C` | current fence counter |
| `+0x30F4` | index buffer object |
| `+0x30F8…` / `+0x3108` | render target surfaces / depth surface |
| `+0x310C + 8n` | stream offset / vertex buffer object |
| `+0x3290` / `+0x3294` | bound pixel / vertex shader (ucode at `+0x0C` / `+0x28`, size `+0x3C` / `+0x258`) |

Texture objects: 0x10-byte resource header followed by the 6-dword texture fetch constant.

### Identified D3D functions

| Address | Function |
|---|---|
| 825EAEC0 | Swap(device, frontBuffer, flags) |
| 825ED210 | Resolve(device, flags, srcRect, destTexture, destPoint, level, slice, clearColor, z, stencil) |
| 825EC990 | Clear(device, count, rects, flags, color, z (f1), stencil) — flags: 0xF targets, 0x10 Z, 0x20 stencil |
| 825EE420 | DrawIndexedVertices(device, prim, baseVertex, startIndex, indexCount) |
| 825EE1E0 | DrawVertices(device, prim, startVertex, vertexCount) |
| 825EDF40 / 825EE1C0 | BeginVertices(device, prim, count, stride) → pointer / EndVertices(device) |
| 825E5670 | internal SetScissor(device, left, top, right, bottom) |
| 825E85C0 / 825E8958 | SetRenderTarget / SetDepthStencilSurface |
| 825E1AE8 / 825E1778 | SetVertexShader / SetPixelShader |
| 825DEB70 | BlockOnFence; 825DF160 KickOff (make ring space); 825DDFA0 GPU interrupt callback |

## Environment variables

| Variable | Effect |
|---|---|
| `Q4_HLE_NATIVE=0` | run q4hle with the normal Xenia GPU (for tracing/comparison) |
| `Q4_HLE_VSYNC=0/1` | overrides the GRAPHICS menu's V-Sync (`q4hle_vsync` cvar, default off) |
| `Q4_HLE_SWAP_WAIT=1` | keep the guest's vblank swap-pacing wait in the null GPU |
| `Q4_HLE_RELAX_DEPTH_EQUAL=1` | relax EQUAL depth tests without depth writes to LESS_EQUAL (breaks alpha-to-mask holes) |
| `Q4_HLE_DUMP_SHADERS=1` | write translated HLSL to `<trace dir>/shaders` |
| `Q4_HLE_CAPTURE_FRAME=n` | dump the device state of every draw in frame n to `capture.bin` |
| `Q4_HLE_RT_HEIGHT=n` | height of the native render targets (default 1024) |
| `Q4_HLE_SCALE=n` | internal resolution scale 1-4 (render targets and resolves at n × the guest size); overrides the GRAPHICS menu's Resolution (`resolution_scale` cvar) |
| `Q4_HLE_STEREO=0/1/2` | overrides the GRAPHICS menu's 3D mode (`q4hle_stereo`: off / half side-by-side / full side-by-side) |
| `Q4_HLE_CRT=0-3` | overrides the GRAPHICS menu's CRT filter (`q4hle_crt_filter`: off / classic / arcade / soft) |
| `Q4_HLE_FXAA=0/1/2` | overrides the GRAPHICS menu's anti-aliasing (`swap_post_effect`) |
| `Q4_HLE_SHADOW=1` | keep Xenia's GPU on the window and run the native renderer offscreen alongside (A/B testing) |
| `Q4_HLE_DUMP_TIMES=t1,t2,…` | at these times (s), write `native_tNNN.bmp` (native frame) and `window_tNNN.bmp` (window contents) |
| `Q4_HLE_STEP_DUMP=lo-hi` | in the captured frame, write colour target 0 after each draw lo…hi as `step_NNNN.bmp` (half resolution) |
| `Q4_HLE_DUMP_BURST=n` | with `Q4_HLE_DUMP_TIMES`: also write the next n presented frames (`native_tNNN_fKK.bmp`), to catch flicker |
| `Q4_PROF=1` | sampling CPU profiler: `prof.txt` (hot guest functions in slow vs normal frames, waits, host code) and `prof_timeline.csv` (fps and worst frame per second) in `Q4_TRACE_DIR`; `Q4_PROF_SLOW_MS` sets the slow-frame threshold. Symbolise with `q4hle.map` (written by the build) and the decompile |
| `Q4_PERF_NATIVE=0` | turn off the CPU fixes in `src/q4_perf.cpp` (precise tic wait, native sin/cos/memcpy) for A/B tests |
| `Q4_RB_THREAD=0` | keep the game's renderer back end on the game thread (default: its own thread, one frame behind the front end; see "Back-end thread") |
| `Q4_RB_MD5R=wait/0` | how MD5R skinning (0x82265000 / 0x82265338) avoids racing the back-end thread: default copy-on-write, `wait` = wait for the back end, `0` = off (shows the flicker) |
| `Q4_FE_JOBS=1` | experimental: create light/shadow interactions (idInteraction::CreateInteraction) on 3 helper threads (`Q4_FE_THREADS=n`); see "Front-end helpers" |
| `Q4_HLE_SYNC=1` | diagnostic: the game waits for every renderer command to finish (no render-thread overlap) |
| `Q4_HLE_CAPTURE_TIME=s` | like `Q4_HLE_CAPTURE_FRAME`, for the first frame after `s` seconds |
| `Q4_TRACE=1`, `Q4_WATCH=1`, `Q4_TRACE_DIR=dir` | D3D call tracing / hang watchdog / output directory |

Logs go to `q4hle.log` in `Q4_TRACE_DIR` (default: the working directory). Every 300 frames the log
has a `renderer cpu (ms/frame)` line: time the renderer spends on the game thread per section
(shader lookup, indices, vertex buffers, constants, textures, D3D11 submission), plus the data hashed
and uploaded per frame. The `timing` line next to it shows the whole frame and the time spent
waiting in Swap (idle headroom).

### Per-frame cost model
- Shaders: looked up by microcode address; every draw compares the microcode bytes with the copy
  the lookup was made from (D3D patches vertex shaders in place for the vertex declaration).
- Vertex buffers: kept on the GPU per fetch constant, split into 16 KB stripes; only stripes a
  draw reads are hashed (once per frame) and only changed stripes are re-uploaded. Stripes that
  are new or changed recently are "hot": every draw compares the bytes it reads, because the game
  reuses temporary vertex memory within a frame.
- Textures: a 64-sample check every frame, a full content hash every 16 frames or when the
  sample changes.
- Constants: uploaded only when they differ from the previous draw.

### Render thread
The renderer is split in two. On the game thread, `Draw`/`Clear`/`Resolve` only *capture*: they
look up shaders, copy indices, verify vertex-buffer stripes, and snapshot the GPU register ranges
the draw reads (constants are deduplicated by content; each new constant block gets a unique id, so
a reused allocation is never mistaken for an unchanged one). The captured commands are pushed in
batches of 32 to a render thread, which owns the D3D11 immediate context and does all
binding, uploads, draws, resolves, the blit and `Present`. `EndFrame` allows at most one frame
in flight. GPU objects freed on the game thread are released on the render thread (`DeferRelease`). Commands are recycled through a free list, so `NewCmd` must reset every field a
capture can leave unset (a stale "non-indexed" flag or index size drew stretched black triangles and
garbled text).
The game thread spends about 0.8–1.1 ms per frame in the renderer, down from about 2.3 ms; the
`renderer cpu` log line reports this as `game-thread capture`.

### Back-end thread (`src/hle/rb_thread.cpp`)
The game's own renderer is split into a front end (visibility, interactions, shadows, dynamic
models; writes a command list into frame memory) and a back end (walks the list and makes the D3D
calls, which the native renderer captures). The 360 build ran both on the game thread. With the
back-end thread, `R_IssueRenderCommands` at the end of a frame hands the command list to a guest
thread and returns; the game thread runs the next game tic and front end while the previous frame
is drawn. Doom 3's dormant SMP support makes this possible:
- two frame-memory buffers; `R_ToggleSmpFrame` swaps the `frameData` pointer between them and
  recycles the one switched to (its deferred triangle-surface frees, memory blocks, command chain)
  after the back end that used it has finished;
- vertex cache calls are serialised and its end-of-frame frees are held until the back end is idle;
  the texture cache's end-of-frame step is deferred the same way;
- level loads, world changes, model/image purges and render shutdown wait for the back end;
- a D3D call from the game thread while the back end draws waits for it (counted in the log as
  `foreign D3D waits`); resource calls (vertex/index buffer create/lock) made under the vertex
  cache lock do not touch the device and run on;
- the back end stays on the game thread while the init-time loading-screen thread (intro videos)
  draws with D3D itself;
- the GL-over-D3D layer's buffer bindings (0x832DDAB0/B4) are per-thread: its buffer and draw
  functions are serialised and run on the game thread with the game thread's own bindings, and
  the orphaned-buffer list released at swap is shared under the same lock;
- `idVertexCache::PurgeAll` and map changes wait for the back end, and so do the paths that change
  shared triangle surfaces in place: freeing a surface outside the deferred list, a model dropping
  its vertex caches, and R_FreeStaticTriSurfVertexCaches (0x822A3D60). The MD5R skinning update
  (0x82265000) calls the last one on every animated surface every frame only to set the same mesh
  link again, so there the call is skipped instead of waited for. The log lists the waits by caller
  (`tri waits by caller`); on the Mars replay they cost about 40 slow frames of the gain;
- the frame-temporary vertex list recycled at end of frame is always the one the back end is not
  drawing from;
- the game's memory-tag scope (a stack of records hanging off one global, 0x830863EC) is made
  per-thread for the back-end thread: `q4_tag_hooks.toml` hooks all 1128 loads/stores of that
  global (generated by `tools/gen_tag.py` from the executable) and `src/q4_tagscope.cpp` redirects
  them to a thread-local slot on that thread. Without this, pushes and pops from both threads
  interleave and the game thread later follows a dead record (crash in AI think).

Effect on the Mars-landing replay (120 Hz, gameplay after the intro, three runs each): frames over
9.5 ms 607-630 -> about 125, lowest one-second average 92.7 fps -> 110 fps. The log reports `rb thread:` lines every 3600 frames
(game-thread D3D waits, frames where the game thread waited for the back end).

Later fixes found with a recording of real play (fights with many animated enemies):
- `R_ReallyFreeStaticTriSurf` called directly (interactions dropping surfaces they just built) is
  turned into a deferred free while the back end draws, instead of waiting for it;
- `idRenderModelOverlay::AddOverlaySurfacesToModel` rebuilt decal surfaces in place; with the
  back-end thread a mid-function hook (`q4_rb_hooks.toml`, `Q4Rb_OverlayRealloc`) takes its other
  branch (free the old surface deferred, build a new one);
- the vertex cache's per-frame bookkeeping no longer records every free (diagnostics cost ~5%).
On that recording (fight at 30-58 s): thread off 94 fps average / 72 minimum / 1660 slow frames;
draw thread now 119 / 106-110 / 140-190.

### Skinned meshes and the back-end thread

MD5R skinning re-skins a model surface in place every frame: it resets the surface's bounds and
fields and rewrites its joint-matrix buffer (+0x8C/+0x90, count +0x94). With the back end drawing
the previous frame from the same surface, it could see half-written data; the view weapon then
flickered unlit (user save, Oct 22). While the back end runs, the update now works on a private
copy of the surface (0xB4 bytes) whose joint buffer alternates between two buffers per surface;
the finished copy is put back in one step. The second buffer is freed with the surface
(R_ReallyFreeStaticTriSurf hooks).

### Front-end helpers (`Q4_FE_JOBS=1`, experimental, off by default)
Before R_AddLightSurfaces' interaction loop (0x8228DCC0), the interactions about to be created
are found (same tests as AddActiveInteraction) and created on guest helper threads, one entity
per job. Made possible by: per-thread shadow-volume statics (18 hooks in `q4_rb_hooks.toml`
rebase the base registers of 0x82FE7928-0x8300FA00 onto a per-thread copy;
`tools/gen_fe_hooks.py`), per-thread tag scope and GL bindings, and locks around frame memory,
the triangle header pool, deferred frees, MakeEmpty and R_DeriveFacePlanes. Result: no
measurable gain (the helpers queue on the shared allocators and locks), and a remaining race in
MD5R mesh data that entities of one model share (crash in 0x82267CD8). Left in for further work.

## Notes on guest behaviour the renderer depends on

- Texture objects allocated in the 0xE0000000 physical heap can hold the CPU address in their fetch
  constant (for example a resolve destination). That heap is mapped 4 KB above its physical range,
  so `tex::Decode` converts every base/mip address to the physical address the GPU uses.
- Resolve results are kept as D3D11 copies of the render target (RGBA). ARGB textures are stored
  BGRA in guest memory and their fetch swizzle is `zyxw`; for a resolved texture the renderer swaps
  that swizzle back so the copy is read with the right channel order.
- Shaders whose control flow only jumps backwards for if/else layouts (no loops) are emitted as an
  unrolled dispatch loop, so pixel shaders keep implicit derivatives (mipmapping). Only shaders with
  real loops sample with `SampleLevel(…, 0)`.
- Known gap: a few vertex shaders are patched by D3D for the bound vertex declaration at draw time
  (their microcode carries `vfetch` with format 0); these read zero positions and draw nothing.

## GRAPHICS menu

`src/q4_graphics.cpp` adds GRAPHICS to the game's Options menu (patched into guis/mainmenu.gui in
memory, next to the Quit Game patch). Rows: Resolution (resolution_scale 1-4: 1280x720 x n),
Display mode (fullscreen), V-Sync (q4hle_vsync in q4hle, vsync in q4), Anti-aliasing
(swap_post_effect: FXAA in the native renderer's final blit), Texture filtering
(anisotropic_override, applied in GetSampler) and FPS counter (q4_show_fps). Changes are staged
(`q4gfx_stage <row> <choice>`) and take effect when APPLY is selected (`q4gfx_apply`, which also
saves q4.toml); leaving with B sends `q4gfx_cancel`. `q4gfx_refresh` (on opening) defines the
q4gfx_1..6 game cvars the page reads. The commands are intercepted in
idCmdSystemLocal::ExecuteTokenizedString (0x8216B400). q4hle defines the SDK GPU plugin's cvars
itself (src/hle/gpu_cvars.cpp) because it does not load that plugin. In q4hle everything applies immediately (a resolution change drops the render targets
between frames); in q4.exe resolution and display mode apply at the next start.

## Stereoscopic 3D (side by side)

GRAPHICS → 3D MODE / 3D DEPTH (`q4hle_stereo`, `q4hle_stereo_depth`). Built from the depth buffer
in the final blit (renderer.cpp "Stereoscopic 3D"): the scene's colour and depth are copied when
drawing on the full-screen target switches from depth-tested (3D) to non-depth-tested (2D
overlays); each eye is then made by shifting scene pixels by a depth-based parallax (search for
the source pixel landing on each output pixel, nearest wins; disocclusions filled from the
background side). Pixels where the final image differs from the scene copy are overlays and stay
at screen depth. Screen plane at 1/z = 0.03 (about 100 units, assuming near plane 3); pop-out
limited to 35% of the maximum parallax. Frames without a 3D scene (menus) are shown flat in both
eyes. FXAA is skipped in 3D mode. Limits: one rendered view (no true per-eye rendering), so edges
show some stretching where hidden background is revealed; transparent effects take the depth of
what is behind them.

## CRT filter

GRAPHICS → CRT FILTER (`q4hle_crt_filter`: 0 off, 1 Classic, 2 Arcade, 3 Soft). The picture
shader of [Cathode](https://github.com/skulitom/CathodeDisplay) by Artem (skulitom), MIT
licence, ported to the native renderer's final blit (renderer.cpp "CRT filter"): an RGB
aperture-grille mask with visible phosphor strips, scanlines, glow, curved glass with a dark
bezel, and vignette. The three choices are Cathode's presets (pitch, scanlines, glow, curvature,
brightness, mask). The grille is drawn at the window's pixel size; its pitch grows with windows
taller than 1080 pixels so the cells stay visible at 1440p and 4K. FXAA, if on, is applied
before the filter. 3D mode is off while the filter is on. Any refresh rate.
