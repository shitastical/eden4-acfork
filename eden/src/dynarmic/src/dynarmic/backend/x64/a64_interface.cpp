// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <chrono>
#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include <bit>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

#ifdef DYNARMIC_PAGE_REUSE_TEST
#include <cstdio>
#include <cstdlib>
extern "C" bool ps4_dump_block;
#endif
namespace Dynarmic::A64 {

using namespace Backend::X64;

#ifdef __OPENORBIS__
// ps4.ini "block_reuse=0" turns this off (read in yuzu_ps4/main.cpp).
extern "C" bool ps4_block_reuse = true;
// Reported by yuzu: kind 0 = flush (amount = blocks kept), 1 = block proved unchanged,
// 2 = block recompiled because its guest code changed, 3 = block dropped for having no proof.
extern "C" void ps4_count_block_reuse(int kind, unsigned long long amount);
#endif

static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf) {
    return RunCodeCallbacks{
        ArgCallback(LookupBlock, reinterpret_cast<u64>(arg)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::AddTicks>(cb)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
    };
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

struct Jit::Impl final {
public:
    Impl(Jit* jit, UserConfig conf)
        : conf(conf)
        , block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf))
        , emitter(block_of_code, conf, jit)
        , polyfill_options(GenPolyfillOptions(block_of_code))
    {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
    }

    ~Impl() = default;

    HaltReason Run() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        // TODO: Check code alignment
        const CodePtr current_code_ptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A64JitState::RSB_PTR_MASK;
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return CodePtr(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }
            return CodePtr((uintptr_t(GetCurrentBlock()) + 15) & ~uintptr_t(15));
        }();
        const HaltReason hr = block_of_code.RunCode(&jit_state, current_code_ptr);
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        const HaltReason hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        std::unique_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        invalid_cache_ranges.add(range);
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    std::string Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        auto const* p = reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(p, p + size);
    }

private:
    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        return this_->GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    CodePtr GetBlock(IR::LocationDescriptor descriptor) {
        if (auto block = emitter.GetBasicBlock(descriptor)) {
#ifdef __OPENORBIS__
            // A flush no longer throws compiled code away; a block that predates the current epoch
            // has to show that the guest words behind it are unchanged before it may run again.
            if (PS4BlockStillValid(descriptor, block->entrypoint)) {
                return block->entrypoint;
            }
            ps4_proofs.erase(descriptor);
            emitter.InvalidateBasicBlocks({descriptor});
            ps4_count_block_reuse(2, 0);
#else
            return block->entrypoint;
#endif
        }

        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            // Immediately evacuate cache
            invalidate_entire_cache = true;
#ifdef __OPENORBIS__
            // This one has to free the code cache for real: keeping the blocks here would leave no
            // room to emit into. (A guest flush reaches this same handler without the flag.)
            ps4_force_full_clear = true;
#endif
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        // JIT Compile
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
        // LocationDescriptor ctor() does important ops (like tflags) do not skip
        auto const arch_descriptor = A64::LocationDescriptor{descriptor};
        ir_block.Reset(arch_descriptor);
#ifdef __OPENORBIS__
        const auto t0 = std::chrono::steady_clock::now();
#endif
        A64::Translate(ir_block, arch_descriptor, get_code, {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct});
#ifdef __OPENORBIS__
        const auto t1 = std::chrono::steady_clock::now();
        // The guest words this block was built from, captured before the optimizer is allowed to
        // touch the block's locations. A64 translation advances the PC by exactly 4 per
        // instruction and never jumps, so [start, end) is the complete, contiguous code span.
        const u64 proof_pc = arch_descriptor.PC();
        const u64 proof_end_pc = A64::LocationDescriptor{ir_block.EndLocation()}.PC();
        u32 proof_words = 0;
        if (proof_end_pc > proof_pc && (proof_end_pc - proof_pc) % 4 == 0) {
            const u64 count = (proof_end_pc - proof_pc) / 4;
            proof_words = count <= 512 ? static_cast<u32>(count) : 0;
        }
#endif
        Optimization::Optimize(ir_block, conf, polyfill_options);
#ifdef __OPENORBIS__
        const auto t2 = std::chrono::steady_clock::now();
        block_of_code.PS4NoteCompile(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
#endif
#ifdef DYNARMIC_PAGE_REUSE_TEST
        {
            const auto desc = emitter.Emit(ir_block);
#ifdef __OPENORBIS__
            PS4RecordProof(descriptor, proof_pc, proof_words);
#endif
            static const bool dump_all = std::getenv("PS4_DUMP_ALL") != nullptr;
            static int dump_index = 0;
            if (dump_all) {
                std::fprintf(stderr, "=== DUMP %d IR\n%s\n", dump_index, IR::DumpBlock(ir_block).c_str());
                char name[128];
                std::snprintf(name, sizeof(name), "build/dump%d.bin", dump_index++);
                FILE* f = std::fopen(name, "wb");
                std::fwrite(reinterpret_cast<const unsigned char*>(desc.entrypoint), 1, desc.size, f);
                std::fclose(f);
            }
            if (ps4_dump_block) {
                ps4_dump_block = false;
                std::fprintf(stderr, "=== IR\n%s\n=== CODE %p size %zu\n", IR::DumpBlock(ir_block).c_str(), (const void*)desc.entrypoint, desc.size);
                const unsigned char* b = reinterpret_cast<const unsigned char*>(desc.entrypoint);
                FILE* f = std::fopen("build/block.bin", "wb");
                std::fwrite(b, 1, desc.size, f);
                std::fclose(f);
            }
            return desc.entrypoint;
        }
#else
        const auto desc = emitter.Emit(ir_block);
#ifdef __OPENORBIS__
        PS4RecordProof(descriptor, proof_pc, proof_words);
#endif
        return desc.entrypoint;
#endif
    }

#ifdef __OPENORBIS__
    // -----------------------------------------------------------------------------------
    // Kept blocks across a guest instruction-cache flush.
    //
    // A full IC IALLU used to discard every translation, so the guest paid to compile exactly the
    // same code again (Run L: 157k blocks thrown away at the first one, ~70 us each). A block is
    // now kept, stamped with the flush epoch it was compiled in, and on the first entry after a
    // flush it has to prove that the guest instruction words behind it are unchanged: two
    // independent 64-bit hashes over the same contiguous word range that was translated. A block
    // that fails the proof is unpatched and recompiled, which is what happened to every block
    // before. So this changes how much we compile and never what the guest computes.
    struct PS4BlockProof {
        u64 hash_a = 0;
        u64 hash_b = 0;
        u64 pc = 0;
        u32 words = 0;
        u32 epoch = 0;
    };
    // Roughly one entry per distinct block; the cap keeps a pathological guest from growing the
    // table without bound (past it, flushes fall back to discarding the cache as before).
    static constexpr size_t PS4_MAX_PROOFS = 400'000;

    void PS4HashSpan(u64 pc, u32 words, u64& a, u64& b) const {
        a = 0xcbf29ce484222325ull;
        b = 0x9e3779b97f4a7c15ull;
        for (u32 i = 0; i < words; ++i) {
            const u64 word = conf.callbacks->MemoryReadCode(pc + 4ull * i).value_or(0);
            a = (a ^ word) * 0x100000001b3ull;
            a ^= a >> 32;
            b = (b + word + 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
            b ^= b >> 29;
        }
    }

    void PS4RecordProof(IR::LocationDescriptor descriptor, u64 pc, u32 words) {
        if (words == 0) {
            // Not a contiguous, size-bounded span: never reuse this block.
            ps4_proofs.erase(descriptor);
            return;
        }
        PS4BlockProof proof;
        proof.pc = pc;
        proof.words = words;
        proof.epoch = ps4_epoch;
        PS4HashSpan(pc, words, proof.hash_a, proof.hash_b);
        ps4_proofs.insert_or_assign(descriptor, proof);
    }

    bool PS4BlockStillValid(const IR::LocationDescriptor& descriptor, CodePtr entrypoint) {
        const auto it = ps4_proofs.find(descriptor);
        if (it == ps4_proofs.end()) {
            ps4_count_block_reuse(3, 0);
            return false;
        }
        PS4BlockProof& proof = it->second;
        if (proof.epoch == ps4_epoch) {
            return true;  // compiled since the last flush: nothing to prove
        }
        u64 a = 0;
        u64 b = 0;
        PS4HashSpan(proof.pc, proof.words, a, b);
        if (a != proof.hash_a || b != proof.hash_b) {
            return false;
        }
        proof.epoch = ps4_epoch;
        // Same guest code, so the direct jumps into this block that the flush took out (they bypass
        // this check, which is why they had to go) can be put back.
        emitter.PS4Repatch(descriptor, entrypoint);
        ps4_count_block_reuse(1, 0);
        return true;
    }
#endif

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

            jit_state.ResetRSB();
            if (invalidate_entire_cache) {
#ifdef __OPENORBIS__
                // Keep the compiled code: only the ways of reaching it without a lookup are cut
                // (direct block-to-block patches, fast dispatch, the return stack above), so every
                // block is proved against guest memory before it runs again. Anything unprovable
                // falls back to the old behaviour of discarding everything.
                if (ps4_block_reuse && !ps4_force_full_clear &&
                    ps4_proofs.size() <= PS4_MAX_PROOFS) {
                    ++ps4_epoch;
                    ps4_count_block_reuse(0, emitter.PS4BlockCount());
                    emitter.PS4StripPatches();
                } else {
                    block_of_code.ClearCache();
                    emitter.ClearCache();
                    ps4_proofs.clear();
                    ++ps4_epoch;
                }
                ps4_force_full_clear = false;
#else
                block_of_code.ClearCache();
                emitter.ClearCache();
#endif
            } else {
                emitter.InvalidateCacheRanges(invalid_cache_ranges);
            }
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

    IR::Block ir_block = {LocationDescriptor(0, FP::FPCR(0), false)};
    const UserConfig conf;
    A64JitState jit_state;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
    bool is_executing = false;
    bool invalidate_entire_cache = false;
#ifdef __OPENORBIS__
    std::unordered_map<IR::LocationDescriptor, PS4BlockProof> ps4_proofs;
    u32 ps4_epoch = 1;
    bool ps4_force_full_clear = false;
#endif
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::mutex invalidation_mutex;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
