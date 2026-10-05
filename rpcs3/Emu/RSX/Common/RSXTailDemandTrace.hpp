#pragma once
// Diagnostic only. Scalar rows reuse the already-prefaulted bounded chain ring.
// A DMA row names the highest-ID published preceding B-base fault, not a job.
#include "readback_chain_diagnostics.hpp"
#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_locking.h"

namespace rsx::tail_demand_trace
{
inline const bool configured = []
{
    const char* option = std::getenv("RPCS3_RSX_TAIL_DEMAND_TRACE");
    return option && std::strcmp(option, "1") == 0 && readback_chain_trace::configured;
}();
inline const bool vm_domain_configured = []
{
    const char* option = std::getenv("RPCS3_RSX_TAIL_VM_DOMAIN_TRACE");
    return option && std::strcmp(option, "1") == 0 && configured;
}();
inline std::atomic<bool> vm_domain_reported{false};
inline constexpr std::uint32_t a_base = 0x37400b80;
inline constexpr std::uint32_t b_base = 0x37784b80;
inline constexpr std::uint32_t plane_bytes = 3686400;
inline constexpr std::uint64_t cohort_limit = 512;
inline std::atomic<std::uint64_t> latest_fault{0}, admitted_faults{0};
inline std::atomic<bool> cap_announced{false};
inline std::atomic<bool> race_announced{false};
inline thread_local std::uint64_t owner_fault = 0, sampled_fault = 0;
inline thread_local std::uint32_t sampled_mask = 0;

struct fault_scope
{
    std::uint64_t previous = 0;
    bool enabled = false;
    fault_scope(std::uint32_t address, bool writing)
    {
        if (!configured) return;
        enabled = true;
        previous = owner_fault;
        owner_fault = 0;
        if (writing || address != b_base) return;
        if (admitted_faults.load(std::memory_order_relaxed) >= cohort_limit)
        {
            if (!cap_announced.exchange(true, std::memory_order_relaxed))
            {
                readback_chain_trace::scope cap("tail_fault_cap");
                cap.auxiliary(admitted_faults.load(std::memory_order_relaxed) + 1, cohort_limit);
            }
            return;
        }
        readback_chain_trace::scope row("tail_fault", true);
        if (!row) return;
        const auto count = admitted_faults.fetch_add(1, std::memory_order_relaxed);
        if (count >= cohort_limit)
        {
            row.finish(~std::uint64_t{0});
            if (!cap_announced.exchange(true, std::memory_order_relaxed))
            {
                readback_chain_trace::scope cap("tail_fault_cap");
                cap.auxiliary(count + 1, cohort_limit);
            }
            return;
        }
        owner_fault = readback_chain_trace::next_id();
        row.event(owner_fault, nullptr);
        row.range(address, 1);
        row.auxiliary(writing, count + 1);
        // Publish row before its key; this key has no mutable side payload.
        row.finish();
        auto current = latest_fault.load(std::memory_order_relaxed);
        while (current < owner_fault && !latest_fault.compare_exchange_weak(
            current, owner_fault, std::memory_order_release, std::memory_order_relaxed)) {}
    }
    ~fault_scope() { if (enabled) owner_fault = previous; }
};

// Called immediately before the original actual memory operation. Arguments
// are already-fetched descriptor/scalar values; no new LS/VM reads. The batched
// helper calls this for each original load, preserving its original order.
inline void observe(std::uint32_t ea, std::uint32_t length, std::uint32_t lsa,
    bool get, std::uint32_t pc, std::uint32_t spu_id, std::uint32_t kind)
{
    if (!configured || !length || readback_chain_trace::closed.load(std::memory_order_relaxed)) return;
    std::uint32_t plane;
    if (ea >= a_base && std::uint64_t(ea) + length <= std::uint64_t(a_base) + plane_bytes) plane = 0;
    else if (ea >= b_base && std::uint64_t(ea) + length <= std::uint64_t(b_base) + plane_bytes) plane = 1;
    else return;
    const auto key = latest_fault.load(std::memory_order_acquire);
    if (!key) return;
    if (sampled_fault != key) { sampled_fault = key; sampled_mask = 0; }
    const std::uint32_t bit = 1u << (plane * 2u + (get ? 0u : 1u));
    if (sampled_mask & bit) return;
    readback_chain_trace::scope row(get ? "tail_dma_get" : "tail_dma_put");
    if (!row) return;
    if (latest_fault.load(std::memory_order_acquire) != key)
    {
        // Registration may wait behind a concurrent fault publication. Retain
        // no stale-cohort row; sampled mask remains unset for the next attempt.
        row.finish(~std::uint64_t{0});
        if (!race_announced.exchange(true, std::memory_order_relaxed))
        {
            readback_chain_trace::scope race("tail_probe_race");
            race.auxiliary(key, latest_fault.load(std::memory_order_acquire));
        }
        return;
    }
    sampled_mask |= bit;
    row.event(key, nullptr);
    row.range(ea, length);
    // These fields are explicitly diagnostic scalars, not Vulkan command IDs.
    row.packet(pc, spu_id, kind);
    row.auxiliary(lsa, plane);
}

template<class Section>
inline void section(const Section& value, std::uint32_t role)
{
    if (!configured || !owner_fault) return;
    const auto range = value.get_section_range();
    const auto confirmed = value.get_confirmed_range();
    const auto locked = value.get_locked_range();
    readback_chain_trace::scope row("tail_section");
    if (!row) return;
    row.event(owner_fault, &value);
    row.range(range.start, range.length());
    row.packet(confirmed.start, confirmed.length(), static_cast<std::uint64_t>(value.get_protection()));
    row.auxiliary(value.last_write_tag, role | (std::uint64_t(value.get_context()) << 8));
    // Diagnostic only: actual atomic VM metadata at this owner observation.
    // It does not pin a mapping or prove future allocation/alias stability.
    if (vm_domain_configured && range.start == a_base && range.length() == plane_bytes &&
        !vm_domain_reported.load(std::memory_order_relaxed))
    {
        readback_chain_trace::scope domain("tail_vm_domain");
        if (domain)
        {
            if (vm_domain_reported.exchange(true, std::memory_order_relaxed))
            {
                domain.finish(~std::uint64_t{0});
            }
            else
            {
                std::uint64_t flags_and = 255, flags_or = 0;
                const auto last_address = std::uint64_t{a_base} + plane_bytes - 1;
                const auto first_page = a_base / 4096;
                const auto last_page = last_address / 4096;
                for (auto page = std::uint64_t{first_page}; page <= last_page; ++page)
                {
                    const auto flags = vm::get_addr_flags(static_cast<std::uint32_t>(page * 4096)).second;
                    flags_and &= flags;
                    flags_or |= flags;
                }
                const auto first_block = a_base / 65536;
                const auto last_block = last_address / 65536;
                std::uint64_t aliases = 0;
                for (auto block = std::uint64_t{first_block}; block <= last_block; ++block)
                    aliases += vm::g_shmem[block].load() != 0;
                domain.event(owner_fault, &value);
                domain.range(a_base, plane_bytes);
                // Scalars, not command IDs: all-page AND/OR, page count.
                domain.packet(flags_and, flags_or, last_page - first_page + 1);
                domain.auxiliary(aliases, last_block - first_block + 1);
            }
        }
    }
    readback_chain_trace::scope lock_row("tail_section_locked");
    lock_row.event(owner_fault, &value);
    lock_row.range(locked.start, locked.length());
    lock_row.auxiliary(role, static_cast<std::uint64_t>(value.is_synchronized()) |
        (static_cast<std::uint64_t>(value.is_flushed()) << 1) |
        (static_cast<std::uint64_t>(value.is_dirty()) << 2));
}
}
