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
- Auto-launch can hide the menus completely. Until the game's first frame the screen
  shows only the boot splash, and ACNH needs about a minute or more to get there, so
  the splash is not a hang: `eden.log` carries a 10 s heartbeat with `stage=waiting
  for guest display` while booting, and `frames.log` appears the moment the game
  presents. Set `autolaunch=0` for the menu.

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

### First run — Run G is fixed, but the splash owns the screen while the game boots

Installed and launched on the console. The 96 MiB crash is gone: four `PS4 JIT:
65552 KiB` caches are allocated without complaint and boot continues past the
launcher into the guest. Measured from the log of that run:

| Time | Event |
|---|---|
| 0.19 s | `Auto-launch: booting /data/ACNH/games/Animal Crossing New Horizons [01006F8002326000][US][v0].nsp` |
| 0.19 s | `PS4 launcher display: 1920x1080 ABGR garlic works` (the menu opened VideoOut; see below) |
| 1.1-1.4 s | four JIT caches, 65,552 KiB each |
| 2.28 s | `PS4 RADV display ready: 1280x720, 3 buffers`, `loading screen shown until the first game frame` |
| 4.67 s | `LoadDiskResources: Total Pipeline Count: 430` — the moved shader cache is being used |
| 7.5 s | `Vulkan driver pipelines cached at: /data/ACNH/cache/shader/01006f8002326000/vulkan_pipelines.bin` |
| 47.5 s | still `composites=0, flips=0, stage=waiting for guest display`, 235k JIT blocks on the busy core |

The app was closed at ~48 s, and the log proves the guest had not presented a single
frame by then — which is why the boot splash was still on screen and looked like art
sitting on top of the game. Two things were wrong, and both are fixed:

1. **The launcher opened its own VideoOut for a screen nobody saw.** With
auto-launch, the menu is skipped in the same instant, so the app opened VideoOut at
1920x1080, flipped the splash, closed it, and the game then opened a second handle at
1280x720. That handoff is how a splash ends up frozen over the graphics: the last
frame of a closed client can stay on the output while the new client flips into
buffers the screen is not showing. The launcher now creates its display lazily —
when auto-launching there is exactly one VideoOut client for the whole run, the
game's own, and the splash is its first frame. The menu still opens one when it is
actually shown.
2. **Nothing told the user the boot was alive.** ACNH needs on the order of a minute
or more to reach its first frame on this port (Run F: renderer init at 47.6 s, and
that was with the menu adding time). The boot splash is static, so a slow boot is
indistinguishable from a frozen overlay. `logs/eden.log` has a 10-second heartbeat
with `stage=` for exactly this; `frames.log` starts the moment the game does present.
3. **The console's own launch screen was never dismissed.** The XMB draws that screen
with the app's `pic1` banner art, and it stays on top of VideoOut until the app asks
for it to be removed — ACNH's own art covering the game is exactly what it looks
like. Nothing in this tree called `sceSystemServiceHideSplashScreen()`. It is now
called once, at the first moment a frame of ours is actually up (the boot splash
flip, or the first game frame if there is no splash picture), so the console's
banner is replaced by our boot screen and then by the game. `eden.log` records
`PS4 launch screen dismissed: 0x0`.

That is also the cheapest way to tell the two failure modes apart: if the screen
switches to the boot splash (game icon, title, **Loading** ring), VideoOut is visible
and only the game's own first frame is pending; if it stays on the banner art, the
console never saw a frame from us and the problem is VideoOut, not the game.

**Still to measure** (needs a run that lasts long enough): the numbers to compare
against Run F are:

| Metric | Run F (before) | Where to read it |
|---|---|---|
| Survives boot | n/a | no `signal 6` in `crash.log`; `Auto-launch: booting ...` in `eden.log` |
| Frame time | 42.8-98.2 ms / 13.4-19.9 fps | `PS4 performance: game X fps ...` every 10 s |
| Frame pacing | unknown | `frames.log`: `awk -F, 'NR>1 && $2>40' | wc -l` counts frames over 40 ms |
| Shader cache | 430 pipelines in ~89 ms | `LoadDiskResources: Total Pipeline Count` |
| Display wait | 0.0 ms/frame | same perf line |

For a pacing distribution: `sort -t, -k2 -n frames.log | tail` gives the worst frames
and `awk -F, 'NR>1{s+=$2;n++}END{print s/n}' frames.log` the mean gap.

## Run I — first gameplay run with numbers

The launch screen fix worked and the game runs. This is the first run with `frames.log`
turned on, so the pacing is measured instead of guessed. 3,573 frames over 160 s.

| Metric | Value |
|---|---|
| Median frame | **33.3 ms = 30 fps** |
| Mean frame | 44.9 ms (22.4 fps) |
| p90 / p95 / p99 | 48.0 / 85.1 / 150.1 ms |
| Worst frame | 4,202.8 ms |
| Frames over 40 ms | 20.4% |
| Time beyond the 30 fps budget | **42.0 s of 160.4 s (26%)** |
| Guest presented | 3,483 flips (no missing presents) |

The distribution is the important part: gaps come in exact multiples of 33.3 ms —
33.3, 50.0, 66.7, 100.0, 150 ms. That is a 30 Hz vsync grid, so the emulator is not
uniformly slow, it is **just missing the deadline**: a frame that needs 34 ms lands at
50 ms (20 fps), one that needs 70 ms lands at 100 ms (10 fps). Windows of the run hit
a clean 30.0 fps; others sit at 10.0 or 22.4 fps for minutes.

### Where the time goes

Per-core JIT counters from the same run (`PS4 JIT` lines):

| Core | Blocks | Self-clears | Translate | Optimize | Emit (mprotect) |
|---|---|---|---|---|---|
| 0 | 440,000 | **4** | 5.5 s | 5.1 s | 35.4 s (7.0 s) |
| 1 | 100,000 | 1 | 1.2 s | 1.3 s | 10.1 s (2.0 s) |
| 2 | 55,000 | 1 | 0.7 s | 0.8 s | 5.8 s (1.1 s) |

46 s of core 0's 202 s (23%) is spent compiling guest code, and the 64 MiB cache
filled four times. A self-clear throws away every translated block and the guest
re-compiles them on the next pass, which is exactly when the 4.2 s and 3.7 s frames
happen. That is the single largest stall in the port.

### Change: give the translating core a bigger cache

The code cache is *flexible* memory shared with the C heap and Vulkan, so it cannot
just be enlarged: 4 x 96 MiB (384 MiB) failed to allocate with `0x8002000c` in Run G
and the `std::bad_alloc` aborted the app. 288 MiB total is known to allocate, so the
first core now gets 96 MiB and the other three keep 64 MiB — the same total that was
proven to work, aimed at the core with 4.4x the block count and 4x the clears.

Two safety nets come with it:

- Allocation failure no longer aborts. The request is halved and retried (96 -> 48 ->
  32 MiB) instead of throwing `std::bad_alloc` out of the JIT constructor, so this
  class of crash cannot happen again whatever the memory situation is.
- `ps4.ini` `jit_cache_mb=N` overrides the size per core (0 = the built-in 96/64), so
  a bigger value can be tried on the console without a rebuild; if it does not fit,
  the log says so and the emulator keeps running with half the request.

`eden.log` now states the size it settled on: `PS4 JIT core 0: 96 MiB code cache`.

### What ACNHRecomp is, and what can be borrowed from it

[ACNHRecomp](https://github.com/Nycolazs/ACNHRecomp) is a **static recompilation** of
the game: the AArch64 code of `main`, `sdk` and `subsdk0` (about 19 million
instructions) is translated ahead of time into C++, built with clang, its NVN graphics
calls are translated to Vulkan and the system services are reimplemented natively. It
is not an emulator and it is not a PS4 program: it is a desktop application (SDL3,
Vulkan, a normal OS), written on the SuperMarioWonderRecomp framework. Running it on a
PS4 would mean porting that port to Orbis — a full second harness — and it is the same
project that was ruled out earlier in favour of the emulator fork.

What is genuinely useful from it is its *findings*, all of which are cheap here:

- it runs 30 fps on an Intel Iris Xe, i.e. the game's own workload is not the problem
  and a 30 fps target is realistic;
- it needed a **shader pre-compilation pass at startup** — the same conclusion this
  port reached with the 430-pipeline disk cache, and the next step here is warming
  those pipelines on a background thread so a new area does not compile mid-frame;
- it added an option to **bypass the CPU-heavy audio reverb**, which is directly
  applicable: audio is emulated on the same weak cores;
- it accelerates **game data decryption with AES-NI**: already true here, so there is
  nothing to borrow. `dynarmic/backend/x64/emit_x64_aes.cpp` emits `aesenclast`,
  `aesdeclast` and `aesimc` when `HostFeature::AES` is set, and the PS4 build compiles
  with `DYNARMIC_ENABLE_CPU_FEATURE_DETECTION=1` (confirmed in the build flags) on a
  Jaguar, which has AES-NI. The log's `Host CPU: ..., AVX=true` shows the same cpuid
  path works on the console. Hypothesis closed.
- its **register cache in the recompiler** is what dynarmic's register allocator
  already is; no action.

None of those need ACNHRecomp's code — they are one-line-to-one-file changes here.

One number from it is worth carrying forward: the single costliest guest function in
their profile was the **I3DL2 audio reverb** (`~20%` of samples, a whole core), and
turning it off took their audio thread from 26% to 11%. Audio is emulated on the same
weak cores here, so if this port has an equally hot guest function it will show up in
a guest-side profile — which is what `profile=on` now collects (Run J).

## Run J — the 96 MiB cache measured, and the per-block mprotect

Second gameplay run with the per-core cache (96/64/64/64 MiB). 3,360 frames over
144.9 s: median 33.12 ms, mean 43.13 ms (23.2 fps), p90 45.6, p95 54.4, p99 139.3,
worst 4,363.8. 1,048 frames (31.2%) over 34 ms; **33.8 s of the 144.9 s beyond the
33.3 ms budget (23.3%)**, against 42.0 s of 160.4 s (26.2%) in Run I. So the bigger
cache is a real but small gain: the 30 Hz grid is still there, and the misses are
still quantised to 50.0 / 66.7 / 100.0 ms.

Core 0: 430,000 blocks, **3 clears instead of 4**, translate 5.3 s, optimize 4.9 s,
emit 34.2 s. The clears happened at 9.5 s (boot), 38.8 s (menu/tutorial) and once
between 38.8 s and 178.8 s. Each sweep is about 157,000 blocks, i.e. the live code of
one phase is already ~100 MiB: a cache big enough to never clear would need ~300 MiB
for that core alone, which the console does not have. Bigger is not the answer; _less
recompiling_ or _cheaper compiling_ is.

### The run is two different problems

The ten-second windows in `eden.log` split cleanly:

| Window | fps | What the game was doing |
|---|---|---|
| 77 s | 8.6 | boot, first frames |
| 87–97 s | 23.6 → 29.9 | settling |
| 107–157 s | 26.8, 25.3, 28.4, 28.8, 28.8, 29.2 | **free roaming: ~28 fps** |
| 167–207 s | 18.5, 19.0, 16.3, 12.0, 17.5 | **loading** (31 composites in one 10 s window) |

Free roaming is 28 fps and needs about +8% to hold 30. The collapse after 160 s is not
translation and not the cache: the block rate stayed at ~4,400 blocks/s and emit was
39% of that window. It is a load — staging buffers jumped to 36 MiB, the guest heap
grew by 56 MiB, and the frame log shows 3 fps for ten seconds. Different problem, and
the profile in the next run is what will name it.

During boot the numbers are the other way round: 11,800 blocks/s at 13 s with emit at
63% of that window, 300,000 blocks translated by 68 s. Starting the game is almost
entirely compilation.

### Change: one mapping instead of two mprotect calls per block

`mprotect 7,105 ms` for 430,000 blocks is 16.5 us per block — two
`sceKernelMprotect` calls for every compiled block (`PS4EnsureWritable` before writing,
`PS4RestoreExecutable` after), and each one is a TLB shootdown that interrupts the
other emulation cores, the GPU thread and the audio thread. On three cores that is
~12 s of the run spent flipping permissions, plus the interference.

The cache is now mapped `PROT_READ | PROT_WRITE | PROT_EXEC` in one call
(`sceKernelMapNamedFlexibleMemory`), so writing compiled code never changes a
protection. Safety, in order of what can go wrong:

- If the kernel refuses RWX, the allocator maps RW exactly as before and clears a
  flag; `PS4EnsureWritable`, `PS4RestoreExecutable` and `PS4Protect` all return early
  when it is set, so the fallback is the previous behaviour line for line.
- The flag is global and only set when _every_ allocation got RWX; a buffer that came
  back RW-only therefore keeps the old window path.
- Emit timing and the every-5,000-block stats are untouched (the counter moved out of
  the protection branch), and the stats line now prints the mprotect **call count**, so
  the next log proves which path ran.

Also switched on for the next run: `profile=on` in `ps4.ini`. The sampler (1 kHz
`SIGPROF` per thread) writes `PROFILE` lines into `eden.log` with the busiest guest PCs
and the guest's own AArch64 code at those PCs, which is the same method ACNHRecomp used
to find its reverb. It costs a little (order 1%) and makes that run's fps slightly
pessimistic; it is on because knowing where the remaining 8% goes is worth more than a
clean number.

## Run K — RWX confirmed, the profile arrives, and the clears are not capacity

Run J's build with the RWX code cache and `profile=on`. 4,081 frames over 247 s:
median 33.12 ms, mean 43.52 (23.0 fps), p90 49, **32.0% of frames over 34 ms**. By ten
second window: 28.9–29.3 fps from 87 s to 147 s, then 25.3, 17.4, 17.2, 14.1, 13.8,
16.0, 16.7, 21.5, 21.8 — the same shape as Run J, and the same two problems.

### The mprotect change worked and bought nothing

```
alloc: PS4 JIT: 98320 KiB in regular flexible memory at 0x2e4024000 (RWX)
PS4LogStats: 430000 blocks, translate 5003 ms, optimize 4860 ms, emit 30003 ms
             (mprotect 0 ms, 0 calls), 3 cache clears, 201268 ms since cache start
```

0 calls, so the emit path is now permission-change free, and emit fell from 34.2 s
(Run J, 145 s) to 30.0 s over a longer run. The frame rate did not move, because core 0
was never saturated by that path — see below. Removing 12 s of overhead from a core
that is 85% busy at 29 fps should have shown up as a small fps gain; that it did not is
the useful part of this run.

### The profile says the display never throttles and no core is pegged

Host-side, summed over 24 ten-second windows per thread (`eboot` = our own `.text`,
`system` = PS4 libraries, `guest-jit` = translated code, `other` = everything else):

| Thread | our C++ | system libs |
|---|---|---|
| CPUCore_0 | 21.3% | 24.3% |
| CPUCore_1 | 12.4% | 47.0% |
| CPUCore_2 | 9.2% | 58.0% |
| CPUCore_3 | 0.1% | 99.9% |
| VulkanWorker | 3.4% | 96.6% |
| GPU | 6.6% | 93.3% |
| HostTiming | 27.2% | 72.8% |

One libkernel address, `0x800003710`, is 88–100% of the system samples on those
threads: it is a blocking wait, and the renderer, the GPU thread, the audio thread and
the third guest core are all parked in it. `composites` equals `flips` in **every**
window of the run, and `display wait 0.0 ms/frame`, `gpu idle 0.0 ms/frame` stay zero,
so the swapchain never makes the game wait for a slot or for the GPU. **The guest sets
the frame rate; nothing on our side paces it.**

Core 0 is the critical path at ~85% busy in the 29 fps windows, and its guest-code
share is the 54.4% that landed in `other`.

### Two mistakes in that profile

- `guest-jit` is **0.0% everywhere** and every translated sample fell into `other`. The
  classifier tested `address >= 0x300000000 && address < 0x400000000`, but the caches
  are allocated at `0x2e4024000`–`0x2f2030000`. So `guestPcOf()` was never consulted
  and `PROFILE code`/`PROFILE page` came out empty: the run could not name a single
  guest function.
- The block map only holds 262,144 registrations and a run compiles 430,000, so after
  the map filled, later blocks were not recorded at all.

Fixed by deriving the span from the registered blocks (`g_jit_low`/`g_jit_high` in
`sortBlocks`) instead of a fixed window, preferring the newest registration for a host
address, and calling `ps4_sampler_reset_blocks()` from `BlockOfCode::ClearCache` so a
flush throws the map away with the code it described. Reporting only — no effect on
emulation.

### Where our own C++ goes (symbolized against `build/eden/bin/eden-ps4`)

Top addresses of our `.text` on core 0: `memset`, `Xbyak::CodeArray::db(int)` (the
emitter, three buckets ≈ 16 points of the printed list), `Dynarmic::IR::Value::IsImmediate`,
`Dynarmic::SpinLock::Unlock`. On `HostTiming`: `Service::HID::NPad::OnUpdate`,
`Core::Timing::CoreTiming::Advance` ×2, `std::__shared_weak_count::lock`,
`__wrap_clock_gettime`. On the GPU thread: `Tegra::DmaPusher::CallMethod`,
`Tegra::DmaPusher::ProcessCommands`, `memcpy`.

Translation is **19.8% of core 0's wall time** (emit 30.0 s + translate 5.0 s +
optimize 4.9 s in 201 s), and it is the largest single thing we control.

### Correction: the clears are the guest asking for them

`ClearCache()` is reached only from `DynarmicCallbacks64::InstructionCacheOperationRaised`
for `IC IALLU`/`IALLUIS` — the guest invalidating its whole instruction cache. ACNH does
it three times in four minutes. **No cache size removes them**: `jit_cache_mb` only
delays a capacity clear, and there were none. The 3 clears are the guest's, and each one
throws away every translation, so ~2/3 of that 30 s of emit is re-compiling code that had
already been compiled. Only a guest range-limited invalidation (which `IC IALLU` cannot
give us) or write-tracking every executable page could avoid it, and both are a bigger
change than the win available elsewhere.

### Could recompiled guest code be grafted in? No.

A static recompilation is not a library of fast functions. Each generated function is
compiled against its own runtime: a fixed guest memory base, its own TLS and thread
state, its own HLE shims for `libSce*`/AVN, and an NVN→Vulkan layer that owns the
process. To call one from dynarmic you would need the guest registers in an x86-64 ABI
shim, identical guest memory addressing, a re-entry trampoline back into the JIT, and a
way to make dynarmic give up invalidation for that range — while the guest still issues
`IC IALLU` and still patches its own code. ACNHRecomp's output is also generated for one
specific memory layout of one specific NSP build. Per function, that is days of bespoke,
unverifiable work, and it breaks the one requirement of this fork: don't break this game.

What is worth taking from it is its *method* — sample, name the hot guest function,
then attack that one thing — and the numbers above say where to attack first: 19.8% of
the critical core is spent compiling guest code, and we cannot even name the guest
functions yet. The next run will, with the range fixed.

## Run L — the profile names the guest, and the answer is "no hot function"

First run with working guest-PC attribution (the range fix in `ps4_sampler.cpp`). 22
heartbeat windows over 228 s, and the sampler now reports `guest-jit` and a `guest`
address list per thread:

| Window | fps |
|---|---|
| 0 s | 2.0 (boot) |
| 10–20 s | 15.9 |
| 20–100 s | 28.9, 29.3, 25.7, 26.4, **30.0**, 29.0, 28.9, 27.2 |
| 110–228 s | 18.4, 15.6, 15.8, 16.1, 15.2, 10.6, 10.3 |

Free roaming still tops out at 30.0 and two windows now sit **on** the cap, so the
remaining work is the collapse after ~110 s, not the plateau.

### Shares of wall time (not of busy time)

| Thread | guest-jit | jit-glue | our C++ | system |
|---|---|---|---|---|
| CPUCore_0 | **47.2%** | 5.1% | 22.5% | 23.3% |
| CPUCore_1 | 18.2% | 17.5% | 12.2% | 50.3% |
| CPUCore_2 | 11.4% | 17.4% | 8.9% | 60.5% |
| CPUCore_3 | 0.0% | 0.0% | 0.2% | 99.8% |
| HostTiming | 0.0% | 0.0% | 27.6% | 72.4% |
| VulkanWorker | 0.0% | 0.0% | 2.9% | 97.1% |

The sampler fires per thread regardless of state, so these are fractions of the 228 s,
which is the number worth having: on the critical core, 47% is the guest's own
instructions, 22.5% is our code, 23.3% is host waits.

### What the guest is actually doing

336 distinct guest PCs were sampled on core 0, and the top twelve are only 58 points
of the run between them — **inside any single window the hottest guest PC is 2–8%**.
Disassembled from the `PROFILE code` dumps (Capstone):

| pc | share | what it is |
|---|---|---|
| `0x823939a4` | 9.4% | byte-wise `memcpy` tail (`ldrb`/`strb`/`cbnz`) |
| `0x84bc98bc` | 8.3% | AES-128 decrypt, ten `aese`/`aesmc` per 16 B |
| `0x850ad904` | 7.4% | `memset`, 32 B per `stp` |
| `0x823d2b58` | 7.3% | word-wise copy loop (`ldr`/`str`/`b.ne`) |
| `0x82bf2400` | 5.3% | virtual dispatch (load vtable, `blr`) |
| `0x85093920` | 4.6% | `strcmp` (`ldrb` pair + `ccmp`) |
| `0x82bf2400`… | | and 330 more, each under 4% |

So **29% of the critical thread is the guest's own hand-written memory loops** and there
is no single hot function to accelerate — the game loads, compares and clears buffers
with the code its compiler emitted into the executable. These are not "hard to emulate"
constructs; each iteration is one load and one store.

That is why they cost so much here: `fastmem_arena` is never assigned on this port, so
`config.fastmem_pointer` is empty and every guest load or store runs dynarmic's *inline
two-level page-table walk* — two dependent loads plus the address arithmetic before the
actual access. Fine once per real access, expensive once per byte copied. A recompiled
build would not change this; a cheaper access path would.

### Our own 22.5% is the emitter

Symbolized: `Xbyak::CodeArray::db(int)`, `Xbyak::CodeGenerator::opRO_MR`,
`boost::container::stable_vector_iterator<Xbyak::Label*>` (the per-branch label list),
`Dynarmic::SpinLock::Unlock`, `Dynarmic::IR::Value::IsImmediate`, and `memset`. This is
the cost of compiling guest code, and it is the largest block of our own time by far.

### The collapse is not compute

The 171 s window has `system 53.9%`, `our C++ 38.3%`, `guest-jit 1.8%`, and its hottest
guest PC is 0.15%: the guest is waiting, not working. 10 fps with an idle core is
streaming and syscalls, which also puts the 8.3% AES loop where you would expect an
on-the-fly content decrypt. That is a different fix from everything above and it is the
reason the run mean (22.4 fps by frame log) disagrees with the plateau (28–30).

### Answer to "should we graft recompiled code in?"

No, and the profile now says why in one line: the guest's time is **diffuse**. There is
no function worth 20% of a frame to replace; the top one is 8.5% in its best window and
the cost is spread over 336 PCs, of which the biggest group is generic buffer-copy code
that a recompilation would compile just as 1:1 as we translate it. A graft would buy
very little and cost a bespoke ABI bridge per function plus invalidation rules we cannot
control. The levers the numbers do support, in order:

1. **Make a guest memory access cheaper than two dependent loads.** That is 29% of the
   critical thread, it is where emulation overhead multiplies by byte count, and it is
   the only change with a plausible 10–20% on the plateau.
2. **Compile less per guest instruction** — emit is 22.5% of the critical thread; the
   hot addresses are inside Xbyak and dynarmic's label bookkeeping.
3. **Stream better** — fixes the collapse, not the plateau.

## Run M — the atomics, not the page walk

Run L left three levers. This run attacks the first one properly, and the result is
that lever 1 was misdiagnosed: a guest load/store in this build is **not** our top cost
in a 30 fps window. The top cost that *is* ours is the guest's atomics.

### Why the page walk cannot be made cheaper (checked, then dropped)

The two-level table walk per access is 13 uops and two dependent loads
(`EmitVAddrLookup`). The obvious trims are all real options, and all of them are either
useless here or a correctness gamble:

- **Dense chunk table so the null-chunk test can go.** A chunk covers
  `2^chunk_bits` pages; the guest needs 39-bit VAs, so a dense table is 1 GiB at any
  chunk size that fits a page table we would still call two-level. Rejected.
- **Wider loads.** The hot loops are `ldrb/strb` (`0x823939a4`), `ldr/str` (`0x823d2b58`)
  and `stp`-style `memset` (`0x850ad904`); merging them is rewriting the guest, not the
  translator. Rejected in Run L as a graft.
- **Drop the range test or the misalignment test.** `only_detect_misalignment` already
  keeps the misalignment test off 8/16-byte accesses, and the range test is what stops
  an out-of-space address from reaching the host. Removing either changes what an
  invalid guest access does, which is exactly the class of change that ends in a kernel
  panic on this console. Rejected on the "nothing breaks" rule.

One uop is recoverable in principle (computing the chunk index as
`vaddr >> (12 + chunk_bits)` instead of `(vaddr >> 12) >> chunk_bits`), but it saves a
single cycle on a path that is dominated by the two dependent loads, so it is not worth
touching the emitter for.

### What the 30 fps windows actually spend on us

From the per-window symbolization, the largest **self-inflicted** group in plateau
windows is the exclusive/atomic path: `Dynarmic::SpinLock::Unlock` alone was 2.3% of a
window, and `ExclusiveMonitor::CheckAndClear`, `Memory::WriteExclusive32`,
`Memory::ReadExclusive32`, `GetPointerImpl`, `CheckMemoryAccess` and the
`DynarmicExclusiveWriteMemory<32>` lambdas push the group to roughly 8–11% of wall time
on core 0. In one line: **the guest's atomics cost more than the guest's loads.**

The cause is that `fastmem_exclusive_access` is false on this port (there is no fastmem
arena to point at), so every `LDXR`/`STXR` becomes a host call into the monitor, and the
monitor took a *process-wide* lock for each one — twice per pair, three cores at once.
Worse, the lock itself was not cheap:

- `SpinLock::Lock` was `std::call_once(...)` plus an **indirect call** into
  JIT-generated code, because the handler was emitted rather than compiled.
- `SpinLock::Unlock` emitted `xor; xchg [ptr], eax; mfence` — the `mfence` is redundant
  after a locked `xchg`, which is already a full barrier.
- `ReadAndMark` called the guest's load **inside the lock**, so a guest `LDXR` held the
  shared lock across a page walk and a guest memory read.

### The change

Four edits, each one argued from the semantics it has to preserve.

1. `dynarmic/common/spin_lock_x64.cpp` — `SpinLock::Lock/Unlock` are now an inline
   test-and-test-and-set: one `lock xchg` (acquire) in the uncontended case, a
   `pause`-padded read loop before the retry, and a plain release store to unlock
   (`xchg` is already a full barrier). The `std::call_once`, the indirect call, the
   one-time code generation and the `mfence` are gone. `EmitSpinLockLock/Unlock` are
   untouched — they are still what the JIT's *inline* fastmem lock path emits, and this
   build never reaches it; deleting them would be an unnecessary change.
2. `dynarmic/interface/exclusive_monitor.h` — `ReadAndMark` performs the guest load
   **before** taking the lock. The load was never part of the reservation: the value it
   produces is only the "expected" operand of the paired `STXR`, and that store compares
   against the value in memory *at the time of the store*, so a write landing between the
   load and the mark can only make the store fail — or match, which is a perfectly valid
   compare-and-swap. The lock now covers exactly the two stores that make up the
   reservation, which is all it was ever needed for.
3. `dynarmic/backend/x64/block_of_code.cpp` — `ClearCache()` logs the block count it is
   about to throw away (`cache clear N with M blocks live`). Run L could only infer the
   clears from a cumulative counter; this makes the re-translation cost of each flush
   directly readable.
4. `yuzu_ps4/main.cpp` + `common/ps4_diagnostics.h` + `core/arm/dynarmic` — the monitor
   counts every guest atomic (reads, writes, and writes that lost their reservation) and
   the 10 s status line reports them (`PS4 atomics: exclusive reads=… writes=… lost
   reservation=…`). This is the measurement that decides the next step: it converts
   "8–11% in the profile" into an ops/second figure we can multiply by a known cost.

Nothing else changed. No scheduling, no timing, no memory-map, no rendering, no frame
pacing, and no change to what a guest instruction computes.

### What to look for in the log

- `PS4 atomics: exclusive reads=… writes=… lost reservation=…` next to the fps line. If
  reads+writes are in the hundreds of thousands per 10 s window, the path is worth
  finishing (the remaining idea is to skip the monitor entirely for uncontended
  single-threaded atomics); if they are a few thousand, it was never the problem and the
  emitter is.
- `lost reservation` as a fraction of writes. A large fraction means the guest is
  spinning on real contention and each retry pays the path again.
- `PS4 JIT …: cache clear N with M blocks live` at each flush, and the 10 s
  `PS4 JIT` line. Comparing the two tells us exactly how much code an `IALLU` is making
  us compile a second time.

### The next lever, and why it is not in this build

The plateau is now close to pinned: 29% guest memory access (cannot be made cheaper
safely, above), 22.5% emitter, ~10% atomics (this run removes a good part of it). The
load-phase collapse, though, is 100% translation, and translation only happens twice
for the same guest code because a full `IC IALLU` throws the entire cache away. The fix
is a **verified block cache**: keep the compiled block, stamp it with a flush epoch, and
when a block from an older epoch is looked up, re-read its guest instruction words and
compare a 128-bit hash of them; matching blocks keep executing, mismatching ones are
unpatched and recompiled. Correctness is preserved by construction — a block is only
reused if the guest bytes it was compiled from are provably unchanged, and anything that
cannot be hashed exactly (a non-contiguous block) falls back to today's behaviour.

That is a change to the JIT's invalidation core, and it needs dynarmic's own
self-modifying-code tests run on the host before it goes anywhere near the console, so
it is deliberately not folded into this measured run.

## Run N — keep the compiled blocks, prove them instead of trusting them

Run M's instrumentation exists to answer one question, and the load-phase numbers already
answer it well enough to act: the collapse is translation, and the same guest code is
translated two or three times per session because a full `IC IALLU` throws the whole cache
away. Run K established those flushes are not a capacity event. So the code is being
compiled again for no reason other than that we cannot prove it did not change.

This run changes that: a flush keeps every compiled block, and each block is **proved
against guest memory** the first time it is entered after the flush.

### Why a proof is possible at all

A64 translation advances the guest PC by exactly four bytes per instruction and never
jumps (`a64_translate.cpp`: one `AdvancePC(4)` per iteration, no other assignment before
the optimiser runs). So a block's source is always the contiguous word range
`[Location().PC(), EndLocation().PC())`, and the instruction count is
`(end - start) / 4`. Two independent 64-bit hashes over those words, taken from
`MemoryReadCode` at compile time, are all we need: re-reading the same range and getting the
same two hashes means every instruction in the block is byte-identical to what was
translated, and matching on both is a 2⁻¹²⁸ event if it is not.

The direction of failure is what makes this safe to ship: a proof can only be too strict.
A block that is not a contiguous, size-bounded span gets no proof and is never reused; a
block whose words changed fails the proof and is thrown away; a block that cannot read its
words fails too. The only way this executes code the guest did not ask for is a double
64-bit hash collision inside one block, which is not a thing that happens.

### The part that actually needed care: how blocks reach each other

Proving a block at lookup time is useless if anything can jump into it without a lookup.
dynarmic has three such paths, and all three bypass `GetBlock` by design:

1. **Direct block-to-block patches** — `Patch()` rewrites a terminal in an already-emitted
   block into a `jmp` straight to the target's entrypoint.
2. **The fast dispatch table** — a 1M-entry table of `{location_descriptor, code_ptr}`
   that the terminal handler hits without a map lookup.
3. **The return stack buffer** — recorded code pointers used on `ret`.

A flush now (a) removes the RSB (`ResetRSB`, as before), (b) empties the fast dispatch
table, and (c) rewrites every patched site back to its dispatcher form. Only then does the
epoch advance, so from that instant every transfer goes through the lookup and therefore
through the proof. The site lists in `patch_information` are **kept**, because they record
which emitted block jumps to which location: when a kept block passes its proof,
`PS4Repatch` puts its inbound direct jumps back, and a block compiled after the flush
re-patches its own sites through the normal `Patch()` path. The direct-jump network is
therefore rebuilt lazily, only for code that proves itself, instead of being lost.

The strip is `Unpatch`, which is the same primitive dynarmic already uses when a data
write invalidates a range — the change is only *when* it runs and that the code survives.

### Two fallbacks, on purpose

- **Arena exhaustion** (`SpaceRemaining() < 1 MiB`) reaches the same handler as a guest
  flush, and must free memory or the emitter would write past the cache. It sets
  `ps4_force_full_clear`, and the flush then discards everything exactly as before. The
  guest-flush path can never take that branch.
- **Proof table cap** (`400k` entries). Past it, flushes go back to discarding the cache,
  so a pathological guest cannot grow the table without bound. The table costs ~70 bytes
  per kept block (~28 MB at the cap), which is the price of the feature.

Both fallbacks keep today's behaviour, and `block_reuse=0` in `/data/ACNH/ps4.ini` turns the
whole thing off (`JIT block reuse`, System Settings → debug) — that build records no proofs
and discards the cache at every flush, exactly as before. If a run of this build misbehaves
in any way, that is the first thing to try, and it costs nothing else.

Keeping `patch_information` across a flush also means those site lists are no longer freed
by a flush, so peak memory for the emitter grows by roughly the number of kept blocks
(one small map entry per location that is jumped to). It is bounded by the block count,
which the code cache already bounds, but it is a real increase and the reason the proof
table needed a cap.

### What the log says

A new 10 s line, next to the atomics one:

```
PS4 block reuse: flushes=N, blocks kept=M, proved=P, recompiled=R
```

- `flushes` — guest instruction-cache flushes that kept their code.
- `blocks kept` — blocks that would have been thrown away (this is the work saved).
- `proved` — blocks that passed the hash and ran again without recompiling.
- `recompiled` — blocks that failed the proof. On the loader this should be small but not
  zero: the guest really does write new code and then flush, and those blocks must be
  rebuilt. If `recompiled` is a large fraction of `kept`, the guest is writing far more
  code than we think and the idea needs revisiting.

Expect the first `blocks kept` to be the big one (the loader burst Run L saw discarding
157k blocks), and the fps windows that follow a flush to stop collapsing to 10–16.

### Honest accounting

This removes re-translation after flushes. It does not make the emitter faster, and it does
not make a guest memory access cheaper — those are still the two costs of the 30 fps
plateau. What it should do is compress the *load* phases, which is where the run-average
fps loses most of its ground against the 29–30 fps plateau.

## Run O — the first measurements of a real M+N run, and two corrected beliefs

Runs M and N were built and shipped, then the console's own logs were pulled
(`/data/ACNH/logs/eden.log`, `eden.previous.log`, `frames.log`) and read. Two things
that earlier runs believed turned out to be wrong, and both are worth stating plainly.

### Run N never fires, and that is not a bug in Run N

Every 10 s window of a 4 minute run prints

```
PS4 block reuse: flushes=0, blocks kept=0, proved=0, recompiled=0
```

so no guest instruction-cache flush happened at all during play. There is not a single
`IC IALLU` line in the session. Run K concluded that the cache clears were the guest
flushing its own code; this run shows the opposite — the two clears that do happen are
**capacity** clears, produced by the emulated arena filling up:

```
cache clear 1 ... (during boot, t~9 s)
cache clear 2 with 207900 blocks live (t=35 s)
cache clear 3 with 382813 blocks live (t=173 s)
```

(`blocks live` is mislabelled: it prints the cumulative number of translated blocks, not
the number resident.) ACNH touches so much code that 96 MiB holds roughly 175–208k
blocks, and a 3 minute session compiles ~430k. Each clear costs ~12 s of re-emission on
the very thread that runs the game, and the frames right after one collapse: 301 frames
per 10 s (the 30 fps cap) before, 131 right after the t=173 s clear, ~230 (23 fps) a
minute later.

So Run N is inert on this title: it targets an event that does not occur in play. It is
kept (it costs nothing when no flush happens) but it is not the answer to anything here.
The memory wall behind the capacity clears is real: the guest reserves 4 GiB of the
console's 4.5 GiB, leaving ~600 MiB of direct memory for the JIT, the texture cache and
the host heap together. Four 96 MiB caches (384 MiB) already failed with `ENOMEM`
(0x8002000c), which is why the sizes are 96/64/64. **The code cache cannot simply be
made bigger** — that is a hard limit of this design, not a setting.

### `jit-glue` on cores 1 and 2 was a profiler artifact

The sampler keeps one registration per compiled block, in a fixed table of 262144
entries shared by all cores. The table is only reset by a cache clear, and core 0 alone
registers ~208k blocks per generation, so it was exhausted inside the first generation:
`CPUCore_1` and `CPUCore_2` registered almost nothing afterwards and *every* sample of
their translated code was reported as unattributed `jit-glue` (45–61% of those cores).
The table is now 1048576 entries (16 MiB, `calloc`, untouched below the high-water
mark), which holds a full generation, so the next run's per-core profile is real.

With that caveat, the gameplay profile (t=231 s, core 0) is:

```
guest-jit 63.5%, jit-glue 3.5%, eboot 9.7%, system 23.1%
```

Host code is 9.7% and completely flat (top address 1.2%), so the emulator's own C++ is
not the cost. Guest PCs are flat too (top 0.9%). The interesting number is the 23.1% of
"system": it is **one address**, `0x800003710`, 22.4% of the core, and the completely
idle `CPUCore_3` sits 100% in the same address. That is our wait primitive, i.e. the
guest thread parked — core 0 is not CPU-starved for that quarter, it is blocked.

### Run O (this build): find out what a frame blocks on

Nothing in a frame can be fixed before it is known whether the guest spends those ~10 ms
per frame waiting for another guest thread (CPU bound, a scheduling problem) or for the
GPU to finish (renderer bound, a completely different fix). So this build measures it at
the one choke point where an emulated thread can park, `KSynchronizationObject::Wait`
behind `svcWaitSynchronization`:

```
PS4 guest waits: N waits, X ms/frame blocked in svcWaitSynchronization
PS4 guest waits:   <class> C calls, T ms total, L ms longest
```

The classes come from `KSynchronizationObject::GetTypeName()` (`KEvent`, `KSemaphore`,
`KReadableEvent`, `KThread`, ...), so the log names the object instead of guessing. Only
waits over 2 ms claim a slot, so the table stays a list of the waits that can move a
frame, and the call itself is unchanged — the timer wraps the existing blocking call.

Together with the corrected block map, the next run answers the last open question:

- a fence/event class dominating ⇒ the renderer is the limiter and the CPU has slack,
- `KThread` dominating ⇒ core 0 waits for the guest's worker threads,
- neither (waits stay small) ⇒ the frame really is ~43 ms of guest execution, and the
  remaining gap to 33.3 ms is translation speed, which no setting can buy back.

## Research — what ACNHRecomp tells us, and two measurements it pointed at

ACNHRecomp (`Nycolazs/ACNHRecomp`, MIT, built on the SuperMarioWonderRecomp framework) is a
*static* recompilation of this exact title: 178,127 functions and 16.4 M AArch64 instructions
of `main` (75 MiB) plus `sdk` (1.67 M) and `subsdk0` (0.9 M), emitted as 1.4 GB of C++ and
compiled for the host, with NVN calls rewritten to Vulkan and 6,200 shader blocks translated
to SPIR-V. It is a different product, not something this fork can adopt: it replaces the
console, has no PS4 port, and its generated C++ needs far more toolchain than an eboot on a
1.6 GHz Jaguar. What follows is what its documentation is worth to an emulator — and one item
that measurably is not worth anything here.

### Its map of the frame loop (borrowed)

`docs/NATIVE60.md` identifies the engine as sead + agl + gsys with `sead::GameFrameworkNx`:

| Address (`main`) | Role |
|---|---|
| `+37b8bf0` | `sead::MainThread` loop: `for (;;) this->procFrame_()` |
| `+37b8c14` | `procFrame_`: `procDraw_`, `procCalc_`, then **wait/present** (vtable+0x70) |
| `+37b8d1c` | `procDraw_`: writes the frame's command lists |
| `+37b90e4` | `procCalc_`: `ControllerMgr`, then the whole method tree (`sead::MethodTreeNode::call`) |
| `+37e3300` | `sead::Thread` message loop of the calc threads (UI `+3c5bb1c`, jobs); presents at `+37b91a4` |

So a slow frame on CPUCore_0 has three candidate regions, and the profile can name the one
that costs 10 ms. It also means the game is not a tight 30 Hz loop we can "just let run":
the wait/present step is *inside* the frame.

### The frame is the clock (borrowed, and it constrains us)

The same document measures that ACNH's logic advances **per frame**, not per second: a line of
dialogue types at ~1.4 characters per frame whether the game presents at 30 or at 60, and
presenting at 60 makes the whole game twice as fast. Only load screens follow the wall clock
(`nn::time`). Its 60 fps work is therefore hundreds of places that count frames.

For us that settles a question worth not re-litigating: **the fork must present at 30 Hz.**
Raising the presentation rate to buy smoothness would speed the game up rather than fix it,
and letting the frame run free would make 31 fps play 3% fast. The target is 33.3 ms, period.

### Its performance findings, mapped onto ours

- *NVN replay is their frame cost*: 31–34 ms of CPU for ~4,900 draws on a Snapdragon 865
  (~6.6 µs a draw), 46% of cycles stalled on memory, and 15–20 ms of a 33 ms frame on an Iris
  Xe. Their parallel-replay attempt failed (45–63 ms of CPU against 28 ms) because per-thread
  descriptor caches and upload rings duplicated the shared work.
  **Ours is not this.** The `GPU` thread spends 13.0% of the frame in host code (~5.7 ms of
  43.7 ms) and `VulkanWorker` 7.4% (~3.2 ms), both diffuse (top address 0.2%), and `radv` is a
  real driver with a real queue. Parallel replay would add risk for a cost we do not have.
- *Software reverb is a whole core on weak hardware*: their audio thread spent ~20% of all
  samples in the I3DL2 reverb before they bypassed it (26% → 11%).
  **We have the same thread**: `DSP_AudioRenderer_Main` (our HLE AudioRenderer,
  `audio_core/adsp/apps/audio_renderer`) runs at 14.1% host + 85.9% parked, i.e. ~6.2 ms of
  host CPU per frame. It is *not* on the critical path unless the guest's main thread waits on
  it — a `KThread`/`KSemaphore` line in Run O's new breakdown would be the first evidence that
  it is, and only then is bypassing the reverb worth a build.
- *Lost wakeups*: their problem 23 is a real kernel semantics bug — `svcWaitSynchronization`
  must keep a `CancelSynchronization` **pending** until a wait consumes it, because that is how
  `nn::os::MultiWait` wakes waiters; dropping it loses wakeups at random. **Checked, ours is
  already correct**: `KThread::WaitCancel` sets `m_wait_cancelled` and
  `KSynchronizationObject::Wait` consumes it before linking waiters
  (`k_synchronization_object.cpp:116`). Nothing to fix.
- *Register cache*: their recompiler fix took a hot function from 9.7 µs to 4.1 µs by keeping
  guest registers in C++ locals instead of reloading them from memory at every call.
  **Dynarmic already does more than this** inside a block, and the residue is measurable and
  small: the hottest host address on the guest cores is
  `ABI_PushRegistersAndAdjustStack` (`abi.cpp`) at 1.8% + 1.2% — about 3% of a core, i.e. the
  per-block prologue/epilogue, not a correctness problem and not a lever worth restructuring
  dynarmic's block model for.
- *Memory budget*: their problem 13 — the engine's `cSys` budget overran because task heaps were
  built from "the rest" and trimmed later in a different order than the console — is the same
  shape as our wall: ACNH sizes heaps off a budget, and ours is a guest that reserves 4 GiB of
  a 4.5 GiB console, which is why the code cache cannot grow.

### What the existing logs already say (no console needed)

`frames.log`, 484 s session, 11,013 frames, mean 43.9 ms:

| gap | share |
|---|---|
| 30–33.4 ms | 33.0% |
| 33.4–36 | 18.1% |
| 36–40 | 8.8% |
| 40–45 | 21.5% |
| 45–50 | 13.1% |
| >50 | 5.5% |

A third of all frames land at 30–33.4 ms — i.e. on the 30 Hz boundary — and the last 40% of
the session has *no* frame under 33.4 ms at all (46.9% at 40–45, 25.9% at 45–50, mean 44.9).
Nothing clusters at 66.6 ms, so frames are not being quantised to every second vsync. The
game is work-bound at ~44 ms with a spread; it is not dropping to 15 fps.

`buffer_wait_us` is the other half of that: `vk_swapchain_ps4.cpp` times the poll in
`AcquireNextImage` (up to 1000 × 1 ms waiting for `sceVideoOutGetFlipStatus` to advance), and
the heartbeat prints `display wait 0.0 ms/frame` for the whole session. **The display path is
not the blocker** — buffers are always free when we ask for one, so the guest's ~10 ms park
cannot be the video-out swap. `gpu_idle 0.0 ms/frame` likewise says the queued-flip path never
falls back to `WaitIdle`.

Which leaves the guest's own synchronisation as the only candidate for the 10 ms, and that is
exactly what Run O measures. The expected reading is a `KThread`/`KSemaphore` class at ~10 ms —
core 0 waiting for `CPUCore_1`/`CPUCore_2` to finish a job — with those cores' real busy share
finally visible now that the block map holds a whole generation.

Per-thread wall-clock shares for a steady gameplay window (t=231 s, 500 Hz sampling of every
thread, so these are shares of the frame, not CPU):

| thread | host | parked |
|---|---|---|
| CPUCore_0 | 9.7% (4.2 ms) | 23.1% (10.1 ms) |
| CPUCore_1 | 12.6% (5.5 ms) | 37.3% |
| CPUCore_2 | 12.2% (5.3 ms) | 47.7% |
| CPUCore_3 | 0% | 100% |
| GPU | 13.0% (5.7 ms) | 86.8% |
| VulkanWorker | 7.4% (3.2 ms) | 92.6% |
| HostTiming | 26.6% (11.6 ms) | 73.4% |
| DSP_AudioRenderer_Main | 14.1% (6.2 ms) | 85.9% |
| VSyncThread, PS4AudioOut | ~0% | ~100% |

`HostTiming` at 26.6% host is the largest single host number in the run, but it is diffuse
(top address 2.4%, then ≤0.7%): it is event dispatch and locking spread across many small
functions, not a hot spot to attack. With eight host cores and four guest threads, extra host
threads do not lengthen the frame unless the main thread waits on one — again, Run O's
breakdown decides.
