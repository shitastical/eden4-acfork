# Animal Crossing: New Horizons on the PS4 — findings and measurements

Everything below is measured on real hardware, not estimated. The goal was to get
ACNH (`01006F8002326000`, US v0) through its loading screen on Eden4, which is the
only title this port cares about.

Status at the time of writing: the game launches, loads, gets in-game, and survives
character creation. Long-session stability is still being measured — see
[Run C](#runs) — and the JIT code-cache regression that killed the last build is
fixed and documented in [Run G](#run-g--the-code-cache-regression). The tree is now
the standalone `eden4-acfork` package described in
[its own section](#this-fork-eden4-acfork).

## The console

| Fact | Value |
|---|---|
| Direct memory | 4608 MiB |
| Guest DRAM requested | 4096 MiB originally, then 3840, **3648 MiB now** (see below) |
| Guest RAM placement | `0x20002c000`, software page tables |
| FTP | `ftp://<console>:2121`, anonymous, GoldHEN |
| Installed app | `/user/app/CUSA23260/` — `app.pkg` (68,681,728 B), `app.pbm`, `app.json`, `app.xml` |
| Package | `CUSA23260`, content id `IV0000-CUSA23260_00-ACNH000000000000`, file `Animal Crossing New Horizons.pkg` |
| Game file | `/data/ACNH/games/Animal Crossing New Horizons [01006F8002326000][US][v0].nsp` (6,694,761,920 B) |
| Settings | `/data/ACNH/ps4.ini` |
| Shader cache | `/data/ACNH/cache/shader/01006f8002326000/` |
| Logs | `/data/ACNH/logs/eden.log`, `frames.log`, `hang.log`, `crash.log` |

## Where the 4608 MiB goes

Guest RAM is allocated eagerly with `sceKernelAllocateDirectMemory` and mapped
once (`common/host_memory.cpp`, the `__OPENORBIS__` branch). It cannot be paged or
reclaimed. Everything else — the JIT code cache, the C heap, and every Vulkan
allocation — competes for what is left:

```
4608 MiB direct
 - guest DRAM                 (4096 originally, 3840 now)
 - JIT code cache             (4 cores x 64 MiB flexible)
 - C heap                     (flexible; ceiling set by the GPU reserve)
 - Vulkan / GPU allocations
```

`ps4_heap_set_overflow_gpu_reserve()` (`yuzu_ps4/main.cpp`) decides how much of the
remainder the C heap is forbidden to touch. That single constant turned out to be
the whole bug.

## This fork: `eden4-acfork`

This tree is no longer a general Eden4: it is a single-game package for ACNH that
can be installed **alongside** a stock Eden4 install because it shares no state
with it.

| Thing | Value | Why |
|---|---|---|
| Repo / binary | `eden4-acfork` | the fork's own name, unchanged in source |
| Package file | `Animal Crossing New Horizons.pkg` | the installed file reads as the game, not as an emulator |
| App title | `Animal Crossing: New Horizons` | what the XMB shows |
| Title id | `CUSA23260` | must match `^[A-Z]{4}[0-9]{5}$`; taken from the game's own id so it reads as ACNH |
| Content id | `IV0000-CUSA23260_00-ACNH000000000000` | `ACNH` label, padded to 16 |
| Icon / banner | `branding/icon0.png` (512x512 RGBA), `branding/pic1.png` (1920x1080) | generated from the desktop art by `build.sh` arguments |
| Data root | `/data/ACNH` | stock Eden4 keeps `/data/EdenPS4`, so the two never collide |

Every path is defined once, in `eden/src/common/ps4_paths.h`; nothing else in the
tree hardcodes a data path. The layout is shaped for FTP browsing:

```
/data/ACNH/
  ps4.ini            settings (same keys as the launcher's menu; hand-editable)
  ui-state.ini       home-menu cursor position
  games/             the Animal Crossing NSP/XCI goes here and nowhere else
  keys/              prod.keys, title.keys
  nand/              save data, system titles, firmware
  cache/shader/      translated GPU pipelines (survives reinstall)
  cache/ui/          cached menu icons and metadata
  logs/              eden.log, frames.log, hang.log, crash.log
```

**Auto-launch.** `autolaunch=1` (the default, and what is set on the console) boots
the game in `games/` as soon as the app starts: the launcher scans, matches the
played file against `Ps4Paths::GameTitleId` (falling back to `01006f8002326000` or
"animal crossing" in the file name before the metadata reader has run), and skips
the menu entirely. If no matching file is found it logs a warning and shows the
normal menu instead of failing. It is a start-up behaviour, not a loop behaviour:
the flag is consumed by the first launch, so quitting the game lands on the menu
rather than booting it again. To reach the emulator menus on the next start, set
`autolaunch=0` in `/data/ACNH/ps4.ini` (or turn it off under Display in System
Settings inside the app) and restart.

**Per-frame log.** `framelog=1` (default) appends `frame,gap_ms` to
`logs/frames.log` from `Swapchain::Present` — the wall-clock gap since the previous
presented frame, i.e. the same number the 10 s heartbeat averages, but per frame, so
a hitch can be located exactly. It is buffered (one flush per 120 frames) and capped
at 8 MiB, after which the old file is renamed to `frames.previous.log`. The verbose
`eden.log` still carries the heartbeat lines (`PS4 performance: game X fps, ...`).

## The crash

Symptom: `uncaught exception std::bad_alloc`, `signal 6`, ~2 minutes in. The crash
log gives a register dump but no allocation site, so the useful evidence is the
10-second heartbeat:

```
PS4 progress: composites=..., flips=..., audio buffers=..., pad=..., stage=...,
              overflow heap=<N> MiB, memory callbacks=<N>
PS4 heartbeat: direct memory query=0x0, largest free block=<N> KiB
```

`overflow heap` is how much the C heap has grown past its initial reservation.
`largest free block` is the biggest contiguous direct-memory allocation still
available. Reading those two together is the entire diagnosis.

### The mechanism

The reserve was 256 MiB with `4 GiB` of guest RAM, described in the source as
*"keep it for the GPU, not the C heap"*. That capped the C heap at ~248 MiB. ACNH
tends its pipeline cache during loading, the heap walked into the cap, and the next
allocation threw — **while direct memory was still free**, which is what made it
look like an OOM when it was really a self-imposed limit.

### Runs

**Run A — reserve 256 MiB, guest 4096 MiB**

| Measurement | Value |
|---|---|
| Survived | ~150 s, died on the loading screen |
| Overflow heap | climbed 0 → 56 → 88 → 120 → 152 → 216 → **248 MiB and stopped** |
| Largest free block | 196–310 MiB |
| Frame rate | 24.4–28.9 fps |
| JIT (busy core) | 370,000 blocks, emit 29.2 s, mprotect 6.2 s, 3 cache clears |

The heap pinned at 248 and never moved again — that is the cap, not demand.

**Run B — reserve 128 MiB, guest 4096 MiB**

| Measurement | Value |
|---|---|
| Survived | **528 s** (3.5x longer) |
| Overflow heap | climbed to **336 MiB and plateaued for ~350 s** |
| Largest free block | **89 MiB** |
| Frame rate | 17.2 fps |
| JIT (busy core) | 460,000 blocks, emit 37.9 s, mprotect 7.2 s, 4 cache clears |

The heap stopped being the constraint — it sat well under its new ~384 MiB ceiling
for six minutes, through naming the character and meeting Timmy and Tommy. The
crash came from the other side: taking 128 MiB for the heap left only 89 MiB of
direct memory, and the first larger allocation died there.

That is the real shape of the problem: **the ledger is full**. Guest RAM is
immovable, so either the heap or the GPU has to go without, unless guest RAM itself
gets smaller.

**Run C — reserve 128 MiB, guest 3840 MiB, applet pool 251 MiB**

| Measurement | Value |
|---|---|
| Survived | **816 s** (13.6 min), died during the island intro on the plane |
| Overflow heap | climbed 152 → 248 → 280 → 368 → 400 → **464 MiB** |
| Largest free block | 151–168 MiB |
| Frame rate | 9.2–18.0 fps |
| JIT (busy core) | 605,000 blocks, emit 52.2 s, mprotect 9.7 s, **6 cache clears** |

No longer a hard ceiling: the heap reached 464 MiB against a ~640 MiB limit. It died
because no *single contiguous* block was big enough any more — 168 MiB free, and the
island intro wanted more than that at once. The reserve was still holding memory the
heap could otherwise have had.

The perf line now also prints `display wait X ms/frame, gpu idle Y ms/frame`: blocked time in
`AcquireNextImage` (GPU plus VideoOut latency) and in the unqueued-flip `WaitIdle` fallback.
Whatever is left of the frame time is the guest's own work, so one run splits the 47 ms
without a profiler running (the profiler itself costs frame rate).

**Run D — reserve 32 MiB, guest 3648 MiB, applet pool 59 MiB**

Expected: ~450 MiB of contiguous free direct memory. Awaiting a long test. Only the
applet pool gives anything up; the application pool is still 3285 MiB.

### Run G — the code cache regression

The first `eden4-acfork` build died differently from every run above: not after
minutes of heap growth, but on the **fourth** JIT core coming up, before the game
even drew a frame.

```
PS4 JIT allocation of 98320 KiB failed: 0x8002000c
terminate called after throwing an instance of 'std::bad_alloc'
... signal 6
```

`0x8002000c` is `SCE_ENOMEM`, and 98320 KiB is `96 MiB + 16 KiB` of Dynarmic code
cache per core. The JIT caches come out of **flexible** memory, and with guest RAM at
3648 MiB the ledger cannot seat four 96 MiB blocks (384 MiB) next to everything else
— the fourth allocation is simply refused. The clear/perf experiment that raised the
cache to 96 MiB was therefore a regression, not a tuning knob: it traded a
one-in-a-hundred cost (`mprotect` after six clears, 9.7 s total) for a guaranteed
crash on boot.

Fix: `code_cache_size` is back to `64_MiB` in
`core/arm/dynarmic/arm_dynarmic_64.cpp`, next to a comment stating why it must not be
raised without shrinking guest RAM first. The FP inaccuracy flags
(`Unsafe_ReducedErrorFP`, `Unsafe_InaccurateNaN`, only under `Auto` and only for
`__OPENORBIS__`) are kept — they do not allocate and cannot cause this.

## Shrinking guest RAM, and the trap in it

The first attempt simply asked for 3584 MiB (a `guest_ram` knob in `ps4.ini`). It
crashed in `SetupPoolPartitionMemoryRegions` before the game even started, and the
knob was removed.

The reason is that the pool sizes are **hardcoded constants** keyed off the memory
*arrangement*, not the requested size (`k_system_control.cpp`):

| Arrangement | Application pool | Applet pool |
|---|---|---|
| 4 GB | 3285 MiB | 507 MiB |

The layout places the pools at the top of DRAM and works downward
(`k_memory_layout.cpp`):

```
pool_end            = dram_end - KTraceBufferSize
application_pool    = pool_end - 3285 MiB
applet_pool_start   = application_pool - applet_pool
                    = dram_end - ktrace - (3285 + applet)
```

At 4096 with 3285 + 507 that leaves 304 MiB of kernel DRAM beneath the pools.
Requesting 3584 without touching the pools keeps the 3792 MiB of pools but only
304 MiB of DRAM to put them in, so the applet pool start falls below the kernel
region and the assert fires. **A different DRAM size only works if the pools move
with it.**

The current build does exactly that: 3840 MiB DRAM, applet pool 507 → 251, which
keeps the sum at 3536. Both sides moved by 256 MiB, so:

```
4096 - ktrace - (3285 + 507) = 304 - ktrace
3840 - ktrace - (3285 + 251) = 304 - ktrace
```

Identical. The kernel region does not move, the layout stays valid by construction,
and — importantly — **the application pool is untouched**, so the emulated console
has exactly the same memory it had before. Only the applet pool shrank, and that
pool exists for HOME-menu applets and the software keyboard.

Risk to watch: the software keyboard used to enter the player's name *is* an
applet. 251 MiB should be far more than it needs, but if name entry hangs, that is
the first thing to suspect.

## Performance

Not yet optimised; this is the baseline to work from. Target is 30 fps.

- In-game: **9–18 fps**, frame time 33–99 ms. The guest asks for 30.
- The JIT is *not* an obvious misconfiguration: `cpu_accuracy` defaults to `Auto`,
  which turns on `unsafe_optimizations`, block linking, fast dispatch and
  `Unsafe_IgnoreGlobalMonitor`. Check that before blaming codegen.
- The loading screen is dominated by JIT work: ~605k blocks translated, **52.2 s in
  emit, 9.7 s of it in `mprotect`**, with **6 cache clears** on the busy core.
- 6 cache clears means the 64 MiB code cache is too small for ACNH's code. Cutting
  it to buy memory would trade a crash for a worse frame rate — don't.
- `memory callbacks` sounds alarming (166 million) but over a 13-minute run that is
  ~0.02% of executed instructions. It is a red herring; don't chase it.
- **Where the 47 ms actually went (Run E, profiler on):** cores 0–2 spent 44–69% of wall
  time executing translated code and the rest parked; the GPU, Vulkan, vsync and audio
  threads were 87–99% *parked*. Emulation speed stayed at 100%. A JIT bottleneck drags
  emulation speed below 100%, so the guest is **blocked on graphics, not short of CPU**.
- Two sampler traps, both of which read as false alarms:
  - `guest-jit 0.0%` does not mean the JIT is idle. This port's code cache lives in direct
    memory, which the sampler has no bucket for, so executed guest code is counted as
    `other` — and `other` is the 44–69% above.
  - `system 74%` is not syscall overhead. Every blocked thread samples at the same park
    address (`0x800003710`), so a high `system` share just means "threads are asleep".
- `gpu_accuracy` is **dead code in this tree** — nothing reads `current_gpu_accuracy`, so
  switching it High/Normal changes nothing. Don't spend a build on it.
- `async_presentation` is real (it spawns a present thread) but is force-disabled under
  `__OPENORBIS__` in `vk_present_manager.cpp` because VideoOut waits for GPU completion;
  re-enabling it needs that wait dealt with first, not a settings flip.
- Frame pacing on a 60 Hz display quantises to 16.7 ms slots: 30 fps needs the frame under
  33.3 ms, and 47 ms drops to 50 ms (20 fps). A 47 ms frame therefore reads as ~21 fps, and
  shaving ~14 ms would snap it to 30 — the target is a 30% cut, not a full rewrite.
- **How to tell whether the frame is CPU- or GPU-bound:** count the `stage=` values
  in the heartbeat. `waiting for next guest display` means the frontend finished
  copying and is idle waiting for the guest to produce a frame; `copying to VideoOut
  buffer` means the reverse. Run C was 71/80 samples *waiting*, with `emulation
  speed 100%` — i.e. the guest clock is on time and the guest itself is slow, but
  that is also what a fence-blocked guest looks like, so attribute it with the
  profiler (`ps4.ini` `profile=on`) before optimising: it logs `PROFILE` lines with
  the hottest guest PCs plus the code at them.
- `use_disk_shader_cache` was `false` upstream, so every launch recompiled every
  pipeline. It is now `true` and `LoadDiskResources` runs after the renderer starts,
  which removes the repeat cost on later launches.
- `use_disk_shader_cache` was `false` upstream, so every launch recompiled every
  pipeline. It is now `true` and `LoadDiskResources` runs after the renderer starts,
  which removes the repeat cost on later launches.

## Known defect, not yet fixed

`core/loader/nca.cpp:100-103` reads the heap size from `npdm_data[0x28]`:

```
Read heap size 0x6e6f69 bytes from main.npdm
Set pointer buffer size to 0x8000 bytes for ProgramID 0x01006f8002326000
```

`0x6e6f69` is ASCII `"ion\0"` — it is reading the wrong field. The real value is at
`aci0_offset + 0x18`. Consequence: `CalculatePointerBufferSize()` picks the <1 GiB
path (`0x8000` instead of `0x10000`) for a program whose heap is larger than that.
Left alone for now because the game runs around it.

## Building and installing

The toolchain is LLVM 21 (apt.llvm.org debs) plus the PacBrew SDK, which is
extracted somewhere writable and presented at `/opt/pacbrew` inside an unprivileged
mount namespace. `ps4vars.sh` is never patched.

```
unshare -rm /path/to/ps4ns.sh bash -c '
  source runtime/scripts/ps4-env.sh
  unset LDFLAGS CFLAGS CXXFLAGS CPPFLAGS LIBS
  ninja -C "$PS4_BUILD_ROOT/build/eden" eden-ps4_eboot -j6'
bash eden/ps4/build.sh --no-configure      # packages "Animal Crossing New Horizons.pkg"
```

The `unset` matters: `build.sh` re-sources `ps4vars.sh`, which re-exports `LDFLAGS`,
so unsetting outside the script has no effect and FFmpeg fails to link with a
double-`--script` relro error. Driving `ninja` directly with those variables unset
sidesteps it.

Then, over FTP: upload the pkg to `/data/pkg/`, install on the console, launch.
`check-oelf.py` should report `PASS` and every package digest should read `OK`.
The output is `~/eden4-build/out/Animal Crossing New Horizons.pkg` (68,681,728 B for
this build). The filename comes from `APP_TITLE` with the colon dropped (`PKG_NAME` in
`build.sh`, overridable with `ACNH_PKG_NAME`), so the package reads as the game rather
than as an emulator. It installs next to a stock Eden4 package with no shared data.

First-run setup on a console that already ran the old build (done once, scripted over
FTP; renames, so the 6.2 GB game file moves instantly):

```
mkdir /data/ACNH/{games,keys,nand,cache/shader,cache/ui,logs}
rename /data/EdenPS4/games/<game>.nsp  -> /data/ACNH/games/<game>.nsp
rename /data/EdenPS4/nand              -> /data/ACNH/nand            # keeps the save
rename /data/EdenPS4/cache/shader/*    -> /data/ACNH/cache/shader/*  # keeps 430 pipelines
copy   /data/EdenPS4/keys/*            -> /data/ACNH/keys/*          # copied, old install stays valid
write  /data/ACNH/ps4.ini              = autolaunch=1 / docked=0 / output=720 / framelog=1
```

### Build problems already solved — don't rediscover them

| Problem | Fix |
|---|---|
| `audio_core/out/*` missing from this fork | Recovered from `juanresendiz813/eden-xbox`, forward-ported `buffer_event->Signal(system.Kernel())` |
| FFmpeg double `--script` relro link error | Unset `LDFLAGS`/`CFLAGS`/`CXXFLAGS`/`CPPFLAGS`/`LIBS`, drive `ninja` directly |
| `nasm 2.11.08` too old for FFmpeg | Built 2.16.03, removed the old binary so it stops shadowing |
| `ModuleNotFoundError: PIL` | `pip install --user --break-system-packages Pillow` |
| `Signal()` signature drift | Updated call sites for the current API |
| Missing `renderer_base.h` include | Added to `yuzu_ps4/main.cpp` |

**`.gitignore` trap:** line 3 is `out/`, which silently excludes
`eden/src/audio_core/out/` — the four files without which the PS4 target does not
link. They are force-added; a plain `git status` will not show them.

## Run F — resolution is not the pacer, and the shader cache already works

Two long-held assumptions were tested and killed in this run.

**720p does not help, because the GPU is not the bottleneck.** Handheld mode
(`docked=0`) makes the game render at 1280x720 instead of 1080p, and output
resolution was dropped to 720 as well. The frame time did not move:

| Config | Game render res | Frame time | fps | `display wait` | `gpu idle` |
|---|---|---|---|---|---|
| docked, output 720 | 1920x1080 | 47.2 ms | 21.2 | 0.0 ms/frame | 0.0 ms/frame |
| handheld, output 720 | 1280x720 | 42.8–98.2 ms | 13.4–19.9 | 0.0 ms/frame | 0.0 ms/frame |

The renderer never waits for a flip buffer and never falls back to `WaitIdle`.
Cutting the pixel count by 44% changed nothing, which is exactly what a CPU-bound
frame looks like. `display wait`/`gpu idle` are the counters added in
`vk_swapchain_ps4.cpp` (`AcquireNextImage` and `Present`), reported by the heartbeat
in `yuzu_ps4/main.cpp`.

**The disk shader cache is not recompiling every launch.** A single run's log shows:

```
[47.635] LoadVulkanPipelineCache: Loaded Vulkan driver pipeline cache
[47.724] LoadDiskResources: Total Pipeline Count: 430
```

430 pipelines read back from disk in ~89 ms, and the word "compil" appears exactly
once in the entire log — an unrelated `Storage buffer failed to track` warning. The
cache files persist at `/data/ACNH/cache/shader/01006f8002326000/`
(`vulkan.bin` 2.5 MB, `vulkan_pipelines.bin` 986 KB). They were moved there from the
old data root, unchanged, when this fork took over — so the cache is already warm.

What "shader compilation" actually costs here is *first-ever* pipelines: anything not
yet in the 430 compiles synchronously the first time it is drawn, which is a hitch,
not a steady-state cost. Because 430 is a small number for a game this large, most of
ACNH's pipeline set is still unvisited, so new areas keep producing one-time hitches.
The fix for that is asynchronous pipeline compilation (upstream yuzu's
"asynchronous shader building"), which does not exist in this tree — `async_shaders`
has zero consumers here.

### Knobs confirmed dead in this tree

Checked by symbol, not by reading settings UIs:

| Knob | Reality |
|---|---|
| `gpu_accuracy` | Only consumed by Qt UI (`yuzu/main_window.cpp`). No renderer reads it. The common "set accuracy to Normal" advice cannot affect this build. |
| `renderer_force_max_clock` | Consumed, but only at `renderer_vulkan.cpp:187` to request a driver clock boost — meaningless while the GPU is 90%+ idle. |
| `gpu_use_fast_time` | Does not exist in this tree at all. |
| `async_shaders` / asynchronous shader building | Does not exist in this tree at all. |
| `vertex_input_dynamic_state` | Already `true` on non-Android; lever spent. |

### Run F build changes

Both are CPU-side, since the GPU has been ruled out:

1. `core/arm/dynarmic/arm_dynarmic_64.cpp` — `cpuopt_unsafe_reduce_fp_error` and
   `cpuopt_unsafe_inaccurate_nan` both default to `true`, but the `Auto` accuracy path
   never read them; only the explicit `Unsafe` path did, so Auto silently dropped them.
   They are now honoured under `Auto` for `__OPENORBIS__`. This gives up bit-exact FP
   error flags and NaN propagation — nothing ACNH depends on — in exchange for cheaper
   float operations, which the game is full of.
2. Same file — `code_cache_size` 64 → 96 MiB. Run C cleared a 64 MiB cache six times in
   fourteen minutes; each clear discards translated code that must be rebuilt. Memory
   is available now (heap plateaued at 368 MiB, ~450 MiB largest free block).

## What the community advice is worth on this port

Researched because it is the obvious place to look, and worth recording so nobody tries
it twice. Almost all of it is written for *stock yuzu on a desktop*, and this tree is a
trimmed fork on fixed hardware.

| Advice | Verdict for this port |
|---|---|
| "Set GPU accuracy to Normal" | Dead. `gpu_accuracy` is read only by Qt UI code (see Run F). |
| "Force maximum clocks" | Dead. Requests a driver clock boost; the GPU is idle. |
| "Use fast GPU time (hack)" | Cannot be applied: the setting does not exist in this tree. It would not help a CPU-bound frame anyway. |
| "Use asynchronous shader building" | Does not exist in this tree. Would remove first-encounter hitches, not raise steady fps. |
| "Turn off SMT" (Steam Deck) | Not applicable as written — PS4 has no SMT — but it is a *scheduling* effect, which is the most interesting remaining clue (cores sit at 44–69% busy while the frame takes 47 ms, which is latency, not throughput). |
| "Extended memory layout (6 GB)" | Wrong direction: the console only has 4608 MiB and guest DRAM is already sized down to survive. |
| "Disable DOF / shadows mod" | ACNH does have such mods, but LayeredFS on yuzu-family emulators is a documented trap: ACNH mods can make the game take "10 minutes to 10 hours" to load. Not worth it against a 47 ms frame that is already CPU-bound (DOF is GPU work). |

ACNH's own settings surface is also thin by design: it is a fixed-30 fps, dynamic
resolution title with no in-game performance options.

### Where the remaining 14 ms actually has to come from

Frame budget at 30 fps is 33.3 ms; we measure 47.2 ms. Per-thread occupancy from the
profiled run is core0 69%, core1 55%, core2 44% — about 1.7 cores of total work. If the
frame were throughput-bound on one thread, that thread would be pinned at 100%. It is
not. So a meaningful part of the 47 ms is *latency between guest threads*, not raw
instruction throughput, which is why the JIT-side knobs keep returning single-digit
percentages.

The three remaining candidates, in order of expected value:

1. **Cross-thread wake latency.** ACNH's task system ping-pongs work between its
   threads. Anything that adds latency per hand-off (host futex wake, scheduler
   re-entry) multiplies by the number of sync points per frame. This is the same effect
   the Steam Deck crowd got by disabling SMT, i.e. a scheduling change, not a codegen
   change.
2. **JIT throughput.** Already at the edge of what this fork exposes: `Auto` is
   `Unsafe_Optimize`-equivalent, fastmem is 64-bit, memory goes through the direct
   table (one dependent load per page, no attribute decoding), block linking is on, and
   cycle counting is off because multicore passes `uses_wall_clock`.
3. **Guest work itself.** Only reducible with game-side mods, which the loader makes
   expensive for this title.

## Iterating on the console

There is no hot-swap path for code. `/user/app/CUSA23260/` holds only `app.pkg`
(a PFS image), and `/user/patch/CUSA23260/` does not exist — the executable is
loaded from the image before any GoldHEN plugin can redirect it. GoldHEN's AFR
plugin can substitute *data* files opened at runtime, which is useful for
`ps4.ini` and caches but not for the binary.

So every code change is: rebuild → package → FTP the 68,681,728-byte
`Animal Crossing New Horizons.pkg` → install.
`ps4.ini` and the shader cache *can* be edited live over FTP.

Operational notes that cost time before:

- Rename works on this FTP server via `-Q "RNFR <path>" -Q "RNTO <path>"`, even for
  multi-GB files. `550 File unavailable` on `RNFR` means the source path is wrong.
- `awk '{print $NF}'` truncates filenames containing spaces, which silently breaks
  anything reasoning about the ACNH NSP.
- Uninstalling the app does not touch `/data/ACNH/` — the NSP, `ps4.ini`, the save
  and the shader cache all survive it, which is also why reinstalling this fork keeps
  its save.
- The fork and a stock Eden4 install are independent: different title ids, different
  data roots. Uninstalling one leaves the other's data untouched.
- Auto-launch can hide the menus completely; if a build seems to "hang" on a black
  screen, check `logs/eden.log` for `Auto-launch: booting ...` before assuming a
  crash, and set `autolaunch=0` to get the menu back.

## Run H — the standalone build

What changed in this build, relative to Run F (the last one that ran on hardware):

| Change | Reason | Risk |
|---|---|---|
| `code_cache_size` 96 MiB -> 64 MiB | 4 x 96 MiB of flexible memory does not fit; Run G crashed with `SCE_ENOMEM` on the 4th core | none; this is the value every earlier run used |
| FP inaccuracy flags kept | free ARM64 speed, no allocation | none seen before Run G, which failed on allocation |
| Package identity `CUSA23260`, title `Animal Crossing: New Horizons` | standalone fork, side by side with stock Eden4 | none |
| Data root `/data/ACNH` (+ moved NSP, nand, shader cache) | keeps both installs' state separate | none; renames, save and 430 pipelines preserved |
| `autolaunch=1` default | boot straight into the game | none; falls back to the menu when no ACNH file is found |
| `framelog=1` default | per-frame `frame,gap_ms` for offline analysis | one buffered line per frame |

Build facts for this pkg: `Animal Crossing New Horizons.pkg`, 68,681,728 B, `check-oelf` PASS,
all 28 package digests `OK`, uploaded to `/data/pkg/`. Console state: prepared and
verified over FTP (tree, NSP, nand, shader cache, keys, `ps4.ini`).

**Not yet measured:** this build has not been launched on the console yet — a pkg
cannot be installed over FTP, so that step is manual (Install Package on the console).
The first run is the decisive check for Run G, and the numbers to compare against
Run F are:

| Metric | Run F (before) | Where to read it |
|---|---|---|
| Survives boot | n/a | no `signal 6` in `crash.log`; `Auto-launch: booting ...` in `eden.log` |
| Frame time | 42.8-98.2 ms / 13.4-19.9 fps | `PS4 performance: game X fps ...` every 10 s |
| Frame pacing | unknown | `frames.log`: `awk -F, 'NR>1 && $2>40' | wc -l` counts frames over 40 ms |
| Shader cache | 430 pipelines in ~89 ms | `LoadDiskResources: Total Pipeline Count` |
| Display wait | 0.0 ms/frame | same perf line |

For a pacing distribution: `sort -t, -k2 -n frames.log | tail` gives the worst frames
and `awk -F, 'NR>1{s+=$2;n++}END{print s/n}' frames.log` the mean gap.
