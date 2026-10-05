#pragma once
// Diagnostic only: no allocations, file writes or clocks inside descriptor/copy loops.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace spu_dma_list_trace
{
constexpr uint32_t slots = 8, capacity = 524288, snapshot_capacity = 64, snapshot_stride = 8192;
enum Field : uint32_t
{
    spu_id, spu_index, entry_pc, cmd, tag, eal, lsa, size, outcome, flags,
    elements, bytes, fetched_groups, get_batches, inline_get, inline_put,
    fallback, zeros, rsx_update_requests, vm_range_calls
};
enum Outcome : uint32_t { incomplete = 0, complete = 1, stalled = 2, replaced = 3 };
enum Flag : uint32_t { outside_end = 1, entry_state = 2, exit_state = 4 };
enum Path : uint32_t { batch_get, direct_get, direct_put, general_fallback };
struct alignas(64) Control { uint64_t sequence, epoch, begin_ns, end_ns, reserved[4]; };
struct alignas(64) Header
{
    char magic[8];
    uint32_t version, header_bytes, slot_bytes, record_bytes, slot_count, record_capacity;
    uint32_t snapshot_capacity, snapshot_bytes;
    uint64_t next_slot, no_slot_calls, init_flags;
    Control control;
    unsigned char reserved[64];
};
struct Group
{
    unsigned char raw[48]; // The existing six-element LS snapshot, never a fresh LS read.
    uint32_t lsa, remaining_bytes, descriptor_ls_offset, valid_elements;
};
struct alignas(64) Record
{
    uint64_t epoch, call_seq, context_cookie, enter_ns, exit_ns, completion;
    uint32_t field[20];
};
struct alignas(64) Snapshot
{
    uint64_t call_seq, block_hash, contiguous_pairs, identical_range_pairs;
    uint32_t eah, pending_mfc, exit_pc, exit_tag, exit_eal, exit_lsa, exit_size, compatible;
    Group groups[3];
};
struct alignas(64) Slot
{
    uint64_t os_tid, published_rows, dropped_rows, captured_calls, reserved[4];
    Record rows[capacity];
    Snapshot captures[snapshot_capacity];
};
struct File { Header header; Slot threads[slots]; };
static_assert(sizeof(Header) == 192 && sizeof(Record) == 128 && sizeof(Snapshot) == 256 && sizeof(Group) == 64);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
inline File* mapping = nullptr;
inline uint64_t now_ns() noexcept
{
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
inline uint64_t read(uint64_t& x, std::memory_order order = std::memory_order_acquire) noexcept
{ return std::atomic_ref<uint64_t>(x).load(order); }
inline void publish(uint64_t& x, uint64_t value) noexcept
{ std::atomic_ref<uint64_t>(x).store(value, std::memory_order_release); }
inline bool initialize(const char* path) noexcept
{
    if (!path || !*path || mapping) return false;
    const int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    if (ftruncate(fd, sizeof(File))) { close(fd); return false; }
    void* ptr = mmap(nullptr, sizeof(File), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) return false;
    std::memset(ptr, 0, sizeof(File)); // Prefault before gameplay; fixed process-lifetime mapping.
    auto* f = static_cast<File*>(ptr);
    std::memcpy(f->header.magic, "SPUDML02", 8);
    f->header.version = 2; f->header.header_bytes = sizeof(Header);
    f->header.slot_bytes = sizeof(Slot); f->header.record_bytes = sizeof(Record);
    f->header.slot_count = slots; f->header.record_capacity = capacity;
    f->header.snapshot_capacity = snapshot_capacity; f->header.snapshot_bytes = sizeof(Snapshot);
    publish(f->header.init_flags, 1); mapping = f;
    return true;
}
inline void bootstrap_from_environment() noexcept
{
    static const bool initialized = [] { return initialize(std::getenv("RPCS3_SPU_DMA_LIST_TRACE_PATH")); }();
    (void)initialized;
}
struct ThreadState
{
    Slot* slot = nullptr; Record* open = nullptr; Snapshot* snapshot = nullptr; uint64_t next = 0;
    uint64_t previous_range = 0; uint32_t previous_ea = 0, previous_size = 0;
    bool attempted = false, previous_valid = false;
};
inline thread_local ThreadState thread;
inline bool window(uint64_t& epoch, uint64_t& begin, uint64_t& end) noexcept
{
    auto& c = mapping->header.control;
    const auto a = read(c.sequence);
    if (a & 1) return false;
    epoch = read(c.epoch, std::memory_order_relaxed);
    begin = read(c.begin_ns, std::memory_order_relaxed);
    end = read(c.end_ns, std::memory_order_relaxed);
    return a == read(c.sequence) && epoch && begin < end;
}
inline Record* enter(void* context, uint32_t id, uint32_t index, uint32_t pc,
    uint64_t hash, uint32_t command, uint32_t command_tag, uint32_t high_ea,
    uint32_t low_ea, uint32_t local, uint32_t list_bytes, uint32_t mfc_size, bool state) noexcept
{
    if (!mapping) return nullptr;
    if (!thread.attempted)
    {
        thread.attempted = true;
        const auto i = std::atomic_ref<uint64_t>(mapping->header.next_slot).fetch_add(1, std::memory_order_relaxed);
        if (i < slots) { thread.slot = &mapping->threads[i]; thread.slot->os_tid = uint64_t(syscall(SYS_gettid)); }
    }
    uint64_t epoch, begin, end;
    if (!window(epoch, begin, end)) return nullptr;
    const auto stamp = now_ns();
    if (stamp < begin || stamp >= end) return nullptr;
    if (!thread.slot)
    {
        std::atomic_ref<uint64_t>(mapping->header.no_slot_calls).fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (thread.open)
    {
        thread.open->field[outcome] = replaced;
        publish(thread.open->completion, 1); thread.open = nullptr;
    }
    if (thread.next == capacity) { ++thread.slot->dropped_rows; return nullptr; }
    auto* r = &thread.slot->rows[thread.next++];
    // Fresh file rows are already zeroed; each row is written only once.
    r->epoch = epoch; r->call_seq = thread.next;
    r->context_cookie = reinterpret_cast<uintptr_t>(context); r->enter_ns = stamp;
    r->field[spu_id] = id; r->field[spu_index] = index; r->field[entry_pc] = pc;
    r->field[cmd] = command; r->field[tag] = command_tag;
    r->field[eal] = low_ea; r->field[lsa] = local; r->field[size] = list_bytes;
    r->field[flags] = state ? uint32_t(entry_state) : 0u;
    thread.snapshot = nullptr;
    if ((r->call_seq - 1) % snapshot_stride == 0)
    {
        const auto i = (r->call_seq - 1) / snapshot_stride;
        if (i < snapshot_capacity)
        {
            auto* snap = &thread.slot->captures[i];
            snap->call_seq = r->call_seq; snap->block_hash = hash;
            snap->eah = high_ea; snap->pending_mfc = mfc_size;
            thread.snapshot = snap; ++thread.slot->captured_calls;
        }
    }
    thread.previous_valid = false; thread.open = r;
    publish(thread.slot->published_rows, thread.next);
    return r;
}
inline void group(Record* r, const void* existing_items, uint32_t local,
    uint32_t remaining, uint32_t descriptor_offset) noexcept
{
    if (!r) return;
    const auto i = r->field[fetched_groups]++;
    if (thread.snapshot && i < 3)
    {
        auto& g = thread.snapshot->groups[i]; std::memcpy(g.raw, existing_items, 48);
        g.lsa = local; g.remaining_bytes = remaining; g.descriptor_ls_offset = descriptor_offset;
        g.valid_elements = remaining / 8 < 6 ? remaining / 8 : 6;
    }
}
inline void set_compatible(Record* r, uint32_t value) noexcept
{
    if (r && thread.snapshot) thread.snapshot->compatible = value;
}
inline void pattern(uint32_t address, uint32_t amount) noexcept
{
    if (!thread.snapshot || !amount) return;
    const uint64_t range = uint64_t(address & ~127u) |
        (uint64_t(((address & 127u) + amount + 127u) & ~127u) << 32);
    if (thread.previous_valid)
    {
        thread.snapshot->contiguous_pairs += uint64_t(thread.previous_ea) + thread.previous_size == address &&
            !(thread.previous_size & 15u) && !(amount & 15u);
        thread.snapshot->identical_range_pairs += thread.previous_range == range;
    }
    thread.previous_ea = address; thread.previous_size = amount; thread.previous_range = range;
    thread.previous_valid = true;
}
inline void element(Record* r, uint32_t address, uint32_t amount, Path path,
    bool rsx_request = false, bool vm_request = false) noexcept
{
    if (!r) return;
    ++r->field[elements]; r->field[bytes] += amount;
    if (!amount) { ++r->field[zeros]; return; }
    ++r->field[path == batch_get || path == direct_get ? inline_get : path == direct_put ? inline_put : fallback];
    r->field[rsx_update_requests] += rsx_request; r->field[vm_range_calls] += vm_request;
    pattern(address, amount);
}
inline void batch(Record* r, const void* existing_items, uint32_t amount) noexcept
{
    if (!r) return;
    ++r->field[get_batches]; r->field[elements] += 6; r->field[bytes] += 6 * amount;
    if (!amount) { r->field[zeros] += 6; return; }
    r->field[inline_get] += 6;
    // Actual EA decoding/pattern metrics only on the bounded stratified sample.
    if (!thread.snapshot) return;
    const auto* raw = static_cast<const unsigned char*>(existing_items);
    for (uint32_t i = 0; i < 6; ++i)
    {
        const auto* q = raw + i * 8 + 4;
        const uint32_t ea = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
        pattern(ea, amount);
    }
}
inline void leave(Record* r, void* context, uint32_t pc, uint32_t command_tag,
    uint32_t low_ea, uint32_t local, uint32_t list_bytes, bool done, bool state) noexcept
{
    if (!r || thread.open != r || r->context_cookie != reinterpret_cast<uintptr_t>(context)) return;
    r->exit_ns = now_ns(); r->field[outcome] = done ? complete : stalled;
    if (thread.snapshot)
    {
        thread.snapshot->exit_pc = pc; thread.snapshot->exit_tag = command_tag;
        thread.snapshot->exit_eal = low_ea; thread.snapshot->exit_lsa = local; thread.snapshot->exit_size = list_bytes;
    }
    if (state) r->field[flags] |= exit_state;
    uint64_t epoch, begin, end;
    if (!window(epoch, begin, end) || epoch != r->epoch || r->exit_ns > end) r->field[flags] |= outside_end;
    publish(r->completion, 1); thread.open = nullptr; thread.snapshot = nullptr;
}
} // namespace spu_dma_list_trace
