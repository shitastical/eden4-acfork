// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/hostloc.h"
#include "dynarmic/common/spin_lock.h"

namespace Dynarmic {

void EmitSpinLockLock(Xbyak::CodeGenerator& code, Xbyak::Reg64 ptr, Xbyak::Reg32 tmp, bool waitpkg) {
    Xbyak::Label start, loop;
    code.jmp(start, code.T_NEAR);
    code.L(loop);

    if (waitpkg) {
        // TODO: this is because we lack regalloc - so better to be safe :(
        code.push(Xbyak::util::rax);
        code.push(Xbyak::util::rbx);
        code.push(Xbyak::util::rdx);
        // TODO: This clobbers EAX and EDX did we tell the regalloc?
        // ARM ptr for address-monitoring

        // XBYAK BUG: code.umonitor(ptr); see issue #255
        // replace once xbyak has been fixed
        code.db(0xF3);
        if (ptr.getIdx() >= 8) code.db(0x41);
        code.db(0x0F); code.db(0xAE);
        code.db(uint8_t((3 << 6) | ((6 & 7) << 3) | (ptr.getIdx() & 7)));

        // tmp.bit[0] = 0: C0.1 | Slow Wakup | Better Savings
        // tmp.bit[0] = 1: C0.2 | Fast Wakup | Lesser Savings
        // edx:eax is implicitly used as a 64-bit deadline timestamp
        // Use the maximum so that we use the operating system's maximum
        // allowed wait time within the IA32_UMWAIT_CONTROL register
        // Enter power state designated by tmp and wait for a write to lock_ptr
        code.mov(Xbyak::util::eax, 0xFFFFFFFF);
        code.mov(Xbyak::util::edx, Xbyak::util::eax);
        // TODO: We can only be here because tmp is 1 already - however we repeatedly overwrite it...
        code.mov(Xbyak::util::ebx, 1);
        code.umwait(Xbyak::util::ebx);
        // CF == 1 if we hit the OS-timeout in IA32_UMWAIT_CONTROL without a write
        // CF == 0 if we exited the wait for any other reason
        code.pop(Xbyak::util::rdx);
        code.pop(Xbyak::util::rbx);
        code.pop(Xbyak::util::rax);
    } else {
        code.pause();
    }
    code.L(start);
    code.mov(tmp, 1);
    /*code.lock();*/ code.xchg(code.dword[ptr], tmp);
    code.test(tmp, tmp);
    code.jnz(loop, code.T_NEAR);
}

void EmitSpinLockUnlock(Xbyak::CodeGenerator& code, Xbyak::Reg64 ptr, Xbyak::Reg32 tmp) {
    code.xor_(tmp, tmp);
    code.xchg(code.dword[ptr], tmp);
    code.mfence();
}

namespace {
inline void Pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
}
}  // namespace

// The exclusive monitor's lock is taken and released twice per guest atomic (LDXR/STXR pair), and
// the guest runs those from three cores at once. This used to be three calls per acquisition --
// std::call_once, then an indirect call into generated code that did `xchg` (lock) or
// `xchg` + `mfence` (unlock) -- because the handlers were emitted rather than compiled. The
// handlers are still emitted for the JIT's own inline fastmem lock path (EmitSpinLockLock), which
// this build never uses: fastmem_exclusive_access is off without a fastmem arena. Doing the
// uncontended case inline with one locked exchange and releasing with a plain release store (xchg
// is already a full barrier, so no mfence) removes that call chain from every atomic op.
void SpinLock::Lock() noexcept {
    if (__atomic_exchange_n(&storage, 1, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    // Contended: spin on a read so the owners' cache line is not invalidated by every waiter, and
    // only retry the exchange once it looks free.
    while (true) {
        while (__atomic_load_n(&storage, __ATOMIC_RELAXED) != 0) {
            Pause();
        }
        if (__atomic_exchange_n(&storage, 1, __ATOMIC_ACQUIRE) == 0) {
            return;
        }
    }
}

void SpinLock::Unlock() noexcept {
    __atomic_store_n(&storage, 0, __ATOMIC_RELEASE);
}

}  // namespace Dynarmic
