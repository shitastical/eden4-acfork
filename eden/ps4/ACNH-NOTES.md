# Animal Crossing: New Horizons on the PS4 — findings and measurements

Everything below is measured on real hardware, not estimated. The goal was to get
ACNH (`01006F8002326000`, US v0) through its loading screen on Eden4, which is the
only title this port cares about.

Status at the time of writing: the game launches, loads, gets in-game, and survives
character creation. Long-session stability is still being measured — see
[Run C](#runs).

## The console

| Fact | Value |
|---|---|
| Direct memory | 4608 MiB |
| Guest DRAM requested | 4096 MiB originally, then 3840, **3648 MiB now** (see below) |
| Guest RAM placement | `0x20002c000`, software page tables |
| FTP | `ftp://<console>:2121`, anonymous, GoldHEN |
| Installed app | `/user/app/EDEN00002/` — `app.pkg` (65,536,000 B), `app.pbm`, `app.json`, `app.xml` |
| Package | `EDEN00002`, content id `IV0000-EDEN00002_00-EDEN400000000000` |
| Game file | `/data/EdenPS4/games/Animal Crossing New Horizons [01006F8002326000][US][v0].nsp` (6,694,761,920 B) |
| Settings | `/data/EdenPS4/ps4.ini` |
| Shader cache | `/data/EdenPS4/cache/shader/01006f8002326000/` |

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
bash eden/ps4/build.sh --no-configure      # packages Eden4-<version>.pkg
```

The `unset` matters: `build.sh` re-sources `ps4vars.sh`, which re-exports `LDFLAGS`,
so unsetting outside the script has no effect and FFmpeg fails to link with a
double-`--script` relro error. Driving `ninja` directly with those variables unset
sidesteps it.

Then, over FTP: upload the pkg to `/data/pkg/`, install on the console, launch.
`check-oelf.py` should report `PASS` and every package digest should read `OK`.

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

## Iterating on the console

There is no hot-swap path for code. `/user/app/EDEN00002/` holds only `app.pkg`
(a PFS image), and `/user/patch/EDEN00002/` does not exist — the executable is
loaded from the image before any GoldHEN plugin can redirect it. GoldHEN's AFR
plugin can substitute *data* files opened at runtime, which is useful for
`ps4.ini` and caches but not for the binary.

So every code change is: rebuild → package → FTP the 65,536,000-byte pkg → install.
`ps4.ini` and the shader cache *can* be edited live over FTP.

Operational notes that cost time before:

- Rename works on this FTP server via `-Q "RNFR <path>" -Q "RNTO <path>"`, even for
  multi-GB files. `550 File unavailable` on `RNFR` means the source path is wrong.
- `awk '{print $NF}'` truncates filenames containing spaces, which silently breaks
  anything reasoning about the ACNH NSP.
- Uninstalling the app does not touch `/data/EdenPS4/` — the NSP, `ps4.ini` and the
  shader cache all survive it.
