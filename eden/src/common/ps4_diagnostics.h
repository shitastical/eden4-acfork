// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <vector>
#include <sys/types.h>
#include "common/common_types.h"
#include "common/logging.h"

extern "C" size_t sceKernelGetDirectMemorySize();
// The SDK declaration passes the output arguments by value.
extern "C" int EdenQueryDirectMemory(off_t, off_t, size_t, off_t*, size_t*)
    asm("sceKernelAvailableDirectMemorySize");

namespace Common::PS4Diagnostics {
// ps4.ini "inline_memory=0": use Dynarmic's memory callbacks instead of the inline page table walk.
inline std::atomic<bool> inline_memory{true};
// ps4.ini "direct_table=0": the inline walk decodes the packed entries instead of the direct values.
inline std::atomic<bool> direct_table{true};
// Sampling profiler (ps4_sampler.cpp); off by default, ps4.ini "profile=on" enables it.
inline std::atomic<bool> profiler{false};
// ps4.ini "jit_cache_mb=N": JIT code cache per core in MiB; 0 keeps the built-in sizes (96 for the
// first core, 64 for the rest). A cache that is too small clears itself, which throws every
// translated block away and re-compiles it -- the biggest single stall this port has.
inline std::atomic<u32> jit_cache_mb{0};
// ps4.ini "framelog=1" (default): one line per presented frame in /data/ACNH/logs/frames.log.
inline std::atomic<bool> framelog{true};
// Guest memory accesses that left the JIT's inline page table walk (one 64-byte slot per core).
inline u64 memory_callbacks[4][8]{};
inline u64 MemoryCallbackTotal() {
    return memory_callbacks[0][0] + memory_callbacks[1][0] + memory_callbacks[2][0] + memory_callbacks[3][0];
}
// ps4.ini "output=1080": 1920x1080 VideoOut buffers instead of 1280x720 (+~13 MiB of direct memory).
inline std::atomic<bool> output_1080{false};
// The launcher's loading screen (1920x1080, 0xAABBGGRR): the game display shows it as its first
// frame, so it stays up while the game boots. Taken (and freed) by whoever exchanges it out.
inline std::atomic<std::vector<u32>*> loading_picture{nullptr};
// What VideoOut accepted for the launcher (PS4 Pro mode may refuse its first setup): the game
// display follows it.
inline std::atomic<bool> videoout_needs_1080{false};
inline std::atomic<bool> videoout_argb{false};
inline std::atomic<u64> composites{};
inline std::atomic<u64> flips{};
inline std::atomic<u64> audio_buffers{};
// Where the renderer's wall time goes, in microseconds: blocked waiting for VideoOut to release a
// display buffer (the GPU plus display latency), and blocked waiting for the GPU to go idle in the
// fallback flip path. Sampled per 10 s window next to the frame rate, they split a slow frame into
// "guest CPU" and "graphics" without a profiler running.
inline std::atomic<u64> buffer_wait_us{};
inline std::atomic<u64> gpu_idle_us{};
// Guest atomics per window (dynarmic's exclusive monitor, see ps4_count_exclusive): LDXR, STXR and
// the STXR that lost its reservation. Every one of them is a host call into the monitor plus a guest
// memory access shared by the three cores, so the rate sizes that part of a slow frame.
inline std::atomic<u64> exclusive_reads{};
inline std::atomic<u64> exclusive_writes{};
inline std::atomic<u64> exclusive_failures{};
// Kept-block cache across a guest instruction-cache flush: how many flushes kept their code, how
// many blocks that saved, how many were proved unchanged on re-entry, and how many had to be
// recompiled because the guest had really changed them.
inline std::atomic<u64> block_reuse_flushes{};
inline std::atomic<u64> block_reuse_kept{};
inline std::atomic<u64> block_reuse_proven{};
inline std::atomic<u64> block_reuse_recompiled{};
inline std::atomic<u64> presented_frames{};
inline std::atomic<const char*> stage{"waiting for guest display"};

inline void Reset() {
    composites = 0;
    flips = 0;
    audio_buffers = 0;
    buffer_wait_us = 0;
    gpu_idle_us = 0;
    presented_frames = 0;
    stage = "waiting for guest display";
}

inline size_t LargestFreeDirect() {
    off_t start{};
    size_t largest{};
    if (EdenQueryDirectMemory(0, sceKernelGetDirectMemorySize(), 16384, &start, &largest) != 0)
        return 0;
    return largest;
}

inline void LogDirectMemory(const char* reason) {
    off_t start{};
    size_t largest{};
    const int result = EdenQueryDirectMemory(0, sceKernelGetDirectMemorySize(), 16384,
                                            &start, &largest);
    LOG_INFO(HW_Memory, "PS4 {}: direct memory query={:#x}, largest free block={} KiB",
             reason, u32(result), largest / 1024);
}
} // namespace Common::PS4Diagnostics
