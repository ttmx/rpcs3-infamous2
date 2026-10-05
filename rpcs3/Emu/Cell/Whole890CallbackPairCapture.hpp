#pragma once
#include "Whole890CallbackTrace.hpp"
// Warmup-only one-shot architectural snapshot; no host rawcontext fabrication.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
namespace whole_890_pair_capture
{
enum Stage : uint64_t { empty=0, taking_entry=1, entry_ready=2, taking_exit=3, complete=4, aborted=5 };
struct Point
{
    uint64_t monotonic_ns;
    uint32_t source_pc, context_pc, expected_return_pc, pending_mfc;
    uint32_t cpu_state, unsavable, mxcsr, abi;
    uint32_t mfc_barrier, mfc_fence, tag_update, tag_mask;
    uint64_t reserved;
};
static_assert(sizeof(Point)==64);
struct alignas(64) Header
{
    char magic[8];
    uint32_t version, header_bytes, file_bytes, payload_bytes;
    uint64_t stage, context_cookie, os_tid, selected_entry_sequence;
    uint32_t spu_id, spu_index, reason, armed;
    char callback_hash[32];
    unsigned char closure_sha256[32];
    alignas(64) Point entry;
    alignas(64) Point exit;
};
static_assert(sizeof(Header)==320);
struct Payload { unsigned char gprs[2048], ls[262144]; };
struct File { Header header; Payload entry, exit; };
inline File* mapping=nullptr;
inline std::atomic<uint64_t> entry_sequence{0};
inline thread_local void* active_context=nullptr;
inline uint64_t now() noexcept { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec); }
inline uint64_t stage() noexcept { return mapping?std::atomic_ref<uint64_t>(mapping->header.stage).load(std::memory_order_acquire):empty; }
inline void publish(Stage value) noexcept { std::atomic_ref<uint64_t>(mapping->header.stage).store(value,std::memory_order_release); }
inline bool code_matches(const unsigned char*ls) noexcept { return whole_890_callback_trace::code_matches(ls); }
inline bool initialize(const char* path) noexcept
{
    if(!path||!*path||mapping)return false;
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return false;
    if(ftruncate(fd,sizeof(File))!=0){close(fd);return false;}
    void* ptr=mmap(nullptr,sizeof(File),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(ptr==MAP_FAILED)return false;
    std::memset(ptr,0,sizeof(File));mapping=static_cast<File*>(ptr);
    auto& h=mapping->header;std::memcpy(h.magic,"SPUP6C31",8);h.version=1;h.header_bytes=sizeof(Header);h.file_bytes=sizeof(File);h.payload_bytes=sizeof(Payload);
    std::memcpy(h.callback_hash,whole_890_callback_trace::selector,sizeof(whole_890_callback_trace::selector));
    const unsigned char digest[32]={0x99,0xa0,0xc6,0x11,0x9c,0xd2,0x0c,0x9f,0x65,0x63,0x4c,0x15,0x21,0x8a,0x03,0x1c,0x80,0x89,0x4c,0xcf,0x4e,0x84,0x19,0x7b,0xb4,0x89,0x80,0x22,0xe4,0xce,0x97,0x09};
    std::memcpy(h.closure_sha256,digest,32);return true;
}
// Once published complete/aborted, subsequent warmup entries do no RMW,
// metadata loads, clocks or code comparisons. Hot diagnostic never sets pairenv.
inline bool needs_entry(void* context) noexcept { return mapping&&(active_context==context||(stage()==empty&&std::atomic_ref<uint32_t>(mapping->header.armed).load(std::memory_order_acquire)==1)); }
inline bool needs_exit(void* context) noexcept { return mapping&&active_context==context; }
inline void enter(void* context,const unsigned char* gprs,const unsigned char* ls,Point point,uint32_t id,uint32_t index) noexcept
{
    if(!needs_entry(context))return;
    if(active_context){mapping->header.reason=1;active_context=nullptr;publish(aborted);return;}
    if(stage()!=empty || point.source_pc!=0x6c30 || point.context_pc!=0x6c30 || point.expected_return_pc!=(point.expected_return_pc&0x3fffc) || point.pending_mfc || point.cpu_state || point.unsavable || !code_matches(ls))return;
    uint64_t expected=empty;
    if(!std::atomic_ref<uint64_t>(mapping->header.stage).compare_exchange_strong(expected,taking_entry,std::memory_order_acq_rel))return;
    const auto sequence=entry_sequence.fetch_add(1,std::memory_order_relaxed)+1;
    auto& h=mapping->header;h.context_cookie=reinterpret_cast<uintptr_t>(context);h.os_tid=uint64_t(syscall(SYS_gettid));h.selected_entry_sequence=sequence;h.spu_id=id;h.spu_index=index;point.monotonic_ns=now();h.entry=point;
    std::memcpy(mapping->entry.gprs,gprs,2048);std::memcpy(mapping->entry.ls,ls,262144);
    active_context=context;publish(entry_ready);
}
inline void abort(void* context,uint32_t reason) noexcept
{
    if(mapping&&active_context==context){mapping->header.reason=reason;active_context=nullptr;publish(aborted);}
}
inline void leave(void* context,const unsigned char* gprs,const unsigned char* ls,Point point) noexcept
{
    if(!mapping||active_context!=context)return;
    // ABI1 is generic BI7478 only. Typed-function r3 SSA return is not a full architectural context.
    if(stage()!=entry_ready || point.abi!=1 || point.source_pc!=0x7478 || point.context_pc!=mapping->header.entry.expected_return_pc || point.expected_return_pc!=mapping->header.entry.expected_return_pc || point.pending_mfc || point.cpu_state || point.unsavable || !code_matches(ls)){abort(context,2);return;}
    publish(taking_exit);point.monotonic_ns=now();mapping->header.exit=point;
    std::memcpy(mapping->exit.gprs,gprs,2048);std::memcpy(mapping->exit.ls,ls,262144);
    active_context=nullptr;publish(complete);
}
}
