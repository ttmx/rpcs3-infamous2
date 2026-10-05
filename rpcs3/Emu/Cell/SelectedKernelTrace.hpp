#pragma once
// Isolated diagnostic implementation. Not installed in the RPCS3 source tree.
// Initialization must finish before gameplay warmup; hot hooks are POD-only.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace selected_kernel_trace
{
constexpr uint32_t version = 1, slots = 8, capacity = 65536;
constexpr uint32_t exact_pc = 0x8ae8, exact_instructions = 245;
constexpr char selector[] = "MJWapFghqTHRnPJR2AbsEtUhS1eQ";
enum Outcome : uint32_t { pending = 0, stock = 1, native = 2, escaped = 3, replaced = 4 };
enum Flag : uint32_t { paused = 1, native_fallback = 2, outside_end = 4 };
struct alignas(64) Control
{
    uint64_t sequence; // odd while controller writes, even when stable
    uint64_t epoch;
    uint64_t begin_ns;
    uint64_t end_ns;
    uint64_t reserved[4];
};
struct alignas(64) Header
{
    char magic[8];
    uint32_t format_version, header_bytes, slot_bytes, record_bytes;
    uint32_t slot_count, record_capacity, selected_pc, selected_instructions;
    uint64_t next_slot, no_slot_calls, init_flags;
    Control control;
    char selected_hash[32];
    unsigned char binary_sha256[32];
};
struct alignas(64) Record
{
    uint64_t epoch, call_seq, os_tid, context_cookie;
    uint64_t enter_ns, exit_ns, paused_ns, pause_begin_ns;
    uint32_t spu_lv2_id, spu_index, runtime_pc, return_pc;
    uint32_t preferred_count, mode, outcome, flags;
    uint64_t reserved[3];
    uint64_t completion; // release publication: 0 incomplete, 1 completed
};
static_assert(sizeof(Record) == 128);
struct alignas(64) Slot
{
    uint64_t os_tid, published_rows, dropped_rows, inactive_calls;
    uint64_t reserved[4];
    Record rows[capacity];
};
struct File { Header header; Slot threads[slots]; };
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
inline File* mapping = nullptr;
inline uint64_t now_ns() noexcept
{
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
inline uint64_t read(uint64_t& v, std::memory_order order = std::memory_order_acquire) noexcept
{
    return std::atomic_ref<uint64_t>(v).load(order);
}
inline void publish(uint64_t& v, uint64_t x) noexcept
{
    std::atomic_ref<uint64_t>(v).store(x, std::memory_order_release);
}
// Explicit bootstrap call only when the diagnostic env is set. A fresh file
// prevents destructive reuse; failures disable tracing and leave stock code.
inline bool initialize(const char* path, const unsigned char* binary_digest = nullptr) noexcept
{
    if (!path || !*path || mapping) return false;
    const int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    if (ftruncate(fd, sizeof(File)) != 0) { close(fd); return false; }
    void* p = mmap(nullptr, sizeof(File), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    // Fixed mapped storage is process-lifetime; no destructor flush or munmap.
    std::memset(p, 0, sizeof(File));
    auto* f = static_cast<File*>(p);
    std::memcpy(f->header.magic, "SPUTR008", 8);
    f->header.format_version = version;
    f->header.header_bytes = sizeof(Header);
    f->header.slot_bytes = sizeof(Slot);
    f->header.record_bytes = sizeof(Record);
    f->header.slot_count = slots;
    f->header.record_capacity = capacity;
    f->header.selected_pc = exact_pc;
    f->header.selected_instructions = exact_instructions;
    std::memcpy(f->header.selected_hash, selector, sizeof(selector));
    if (binary_digest) std::memcpy(f->header.binary_sha256, binary_digest, 32);
    publish(f->header.init_flags, 1);
    mapping = f;
    return true;
}
struct ThreadState { Slot* slot = nullptr; Record* open = nullptr; uint64_t next = 0; bool attempted = false; };
inline thread_local ThreadState thread;
inline bool window(uint64_t& epoch, uint64_t& begin, uint64_t& end) noexcept
{
    auto& c = mapping->header.control;
    const auto a = read(c.sequence);
    if (a & 1) return false;
    epoch = read(c.epoch, std::memory_order_relaxed);
    begin = read(c.begin_ns, std::memory_order_relaxed);
    end = read(c.end_ns, std::memory_order_relaxed);
    const auto b = read(c.sequence);
    return a == b && epoch && begin < end;
}
// Called only by an exact-hash selected JIT entry after code verification and
// its top-level state gate, before chunk entry/GPR loads. No hot allocation.
inline void enter(void* context, uint32_t spu_id, uint32_t spu_index,
    uint32_t pc, uint32_t preferred_count, uint32_t mode) noexcept
{
    if (!mapping) return;
    if (!thread.attempted)
    {
        thread.attempted = true;
        const auto i = std::atomic_ref<uint64_t>(mapping->header.next_slot).fetch_add(1, std::memory_order_relaxed);
        if (i < slots)
        {
            thread.slot = &mapping->threads[i];
            thread.slot->os_tid = uint64_t(syscall(SYS_gettid));
        }
    }
    uint64_t epoch, begin, end;
    if (!window(epoch, begin, end)) return;
    const auto stamp = now_ns();
    if (stamp < begin || stamp >= end) return;
    if (!thread.slot)
    {
        std::atomic_ref<uint64_t>(mapping->header.no_slot_calls).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto* s = thread.slot;
    if (thread.open)
    {
        // Missing exit is never converted to a normal timing duration.
        thread.open->outcome = replaced;
        publish(thread.open->completion, 1);
        thread.open = nullptr;
    }
    if (thread.next == capacity) { ++s->dropped_rows; return; }
    auto* r = &s->rows[thread.next++];
    *r = {};
    r->epoch = epoch; r->call_seq = thread.next; r->os_tid = s->os_tid;
    r->context_cookie = reinterpret_cast<uintptr_t>(context);
    r->enter_ns = stamp; r->spu_lv2_id = spu_id; r->spu_index = spu_index;
    r->runtime_pc = pc; r->preferred_count = preferred_count; r->mode = mode;
    thread.open = r;
    publish(s->published_rows, thread.next);
}
inline void fallback() noexcept { if (thread.open) thread.open->flags |= native_fallback; }
// Cold hooks only; bracket an in-place check_state pause to separate resumed
// wall duration from active interval union. Escape uses abort() below.
inline void pause_begin() noexcept
{
    if (thread.open && !thread.open->pause_begin_ns)
        thread.open->pause_begin_ns = now_ns();
}
inline void pause_end() noexcept
{
    if (thread.open && thread.open->pause_begin_ns)
    {
        thread.open->paused_ns += now_ns() - thread.open->pause_begin_ns;
        thread.open->pause_begin_ns = 0;
        thread.open->flags |= paused;
    }
}
inline void leave(void* context, uint32_t return_pc, Outcome outcome) noexcept
{
    auto* r = thread.open;
    if (!r || r->context_cookie != reinterpret_cast<uintptr_t>(context)) return;
    r->exit_ns = now_ns(); r->return_pc = return_pc; r->outcome = outcome;
    uint64_t epoch, begin, end;
    if (!window(epoch, begin, end) || epoch != r->epoch || r->exit_ns > end)
        r->flags |= outside_end;
    publish(r->completion, 1);
    thread.open = nullptr;
}
inline void abort(void* context) noexcept { leave(context, 0, escaped); }
} // namespace selected_kernel_trace
