// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <boost/container/static_vector.hpp>

#include <dynarmic/common/spin_lock.h>

namespace Dynarmic {

using VAddr = std::uint64_t;
using Vector = std::array<std::uint64_t, 2>;

#ifdef __OPENORBIS__
// Counting every guest atomic for the PS4 build's per-window stats (yuzu collects them). The JIT
// calls the monitor directly, so this is the only place that sees all exclusive ops; the definition
// lives in core/arm/dynarmic/arm_dynarmic_64.cpp. kind: 0 = exclusive read, 1 = exclusive write,
// 2 = write that lost its reservation.
extern "C" void ps4_count_exclusive(int kind);
#endif

class ExclusiveMonitor {
public:
    /// @param processor_count Maximum number of processors using this global
    ///                        exclusive monitor. Each processor must have a
    ///                        unique id.
    explicit ExclusiveMonitor(size_t processor_count);

    size_t GetProcessorCount() const;

    /// Marks a region containing [address, address+size) to be exclusive to
    /// processor processor_id.
    template<typename T, typename Function>
    T ReadAndMark(size_t processor_id, VAddr address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

        // The load is not part of the reservation: it used to run inside the lock, so every guest
        // LDXR held the shared monitor lock across a page table walk and a guest memory read,
        // serialising the three cores. The value only becomes the "expected" for the paired STXR,
        // and that store compares against memory at the time of the store, so a write that lands
        // between this load and the mark can only fail the store (or match it, which is a valid
        // compare-and-swap). Marking afterwards keeps the same reservation semantics.
        const T value = op();

        Lock();
        exclusive_addresses[processor_id] = masked_address;
        std::memcpy(exclusive_values[processor_id].data(), &value, sizeof(T));
        Unlock();
#ifdef __OPENORBIS__
        ps4_count_exclusive(0);
#endif
        return value;
    }

    /// Checks to see if processor processor_id has exclusive access to the
    /// specified region. If it does, executes the operation then clears
    /// the exclusive state for processors if their exclusive region(s)
    /// contain [address, address+size).
    template<typename T, typename Function>
        requires std::is_trivially_copyable_v<T>
    bool DoExclusiveOperation(size_t processor_id, VAddr address, Function op) {
#ifdef __OPENORBIS__
        ps4_count_exclusive(1);
#endif
        if (!CheckAndClear(processor_id, address)) {
#ifdef __OPENORBIS__
            ps4_count_exclusive(2);
#endif
            return false;
        }

        T saved_value;
        std::memcpy(&saved_value, exclusive_values[processor_id].data(), sizeof(T));
        const bool result = op(saved_value);

        Unlock();
        return result;
    }

    /// Unmark everything.
    void Clear();
    /// Unmark processor id
    void ClearProcessor(size_t processor_id);

private:
    bool CheckAndClear(size_t processor_id, VAddr address);

    void Lock();
    void Unlock();

    friend volatile int* GetExclusiveMonitorLockPointer(ExclusiveMonitor*);
    friend size_t GetExclusiveMonitorProcessorCount(ExclusiveMonitor*);
    friend VAddr* GetExclusiveMonitorAddressPointer(ExclusiveMonitor*, size_t index);
    friend Vector* GetExclusiveMonitorValuePointer(ExclusiveMonitor*, size_t index);

    static constexpr VAddr RESERVATION_GRANULE_MASK = 0xFFFF'FFFF'FFFF'FFFFull;
    static constexpr VAddr INVALID_EXCLUSIVE_ADDRESS = 0xDEAD'DEAD'DEAD'DEADull;
    static constexpr size_t MAX_NUM_CPU_CORES = 4; // Sync with src/core/hardware_properties
    boost::container::static_vector<VAddr, MAX_NUM_CPU_CORES> exclusive_addresses;
    boost::container::static_vector<Vector, MAX_NUM_CPU_CORES> exclusive_values;
    SpinLock lock;
};

}  // namespace Dynarmic
