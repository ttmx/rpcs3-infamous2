#pragma once
// Optional CPU flip/submission timestamps. No GPU queries or per-frame file I/O.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
namespace vk_frame_interval_trace {
constexpr uint32_t capacity=65536;
struct alignas(64) Header {
 char magic[8]; uint32_t version,row_size,capacity_rows,pid;
 uint64_t writer_tid,arm_seq,start_ns,end_ns,used,dropped,foreign_writer,initialized_ns,completed_emu_flips;
 uint64_t reserved[4];
};
struct Row {uint64_t sequence,ns,flip_id,payload;uint32_t kind,flags,buffer,reserved;};
static_assert(sizeof(Header)==128 && sizeof(Row)==48);
enum : uint32_t {begin=1,completed=2,present=3,aborted=4};
enum : uint32_t {emulated=1,skipped=2,unavailable=4};
inline uint64_t now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec;}
struct Writer {
 Header* h=nullptr;Row* rows=nullptr;uint64_t tid=0,next_flip=0;
 Writer(){const char* enabled=std::getenv("RPCS3_VK_FRAME_INTERVAL_TRACE"),*path=std::getenv("RPCS3_VK_FRAME_INTERVAL_TRACE_PATH");if(!enabled||std::strcmp(enabled,"1")||!path||!*path)return;
  const int fd=::open(path,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return;constexpr size_t bytes=sizeof(Header)+sizeof(Row)*capacity;
  if(ftruncate(fd,bytes)){close(fd);return;}void* map=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);if(map==MAP_FAILED)return;
  h=static_cast<Header*>(map);rows=reinterpret_cast<Row*>(h+1);std::memset(h,0,bytes);h->version=1;h->row_size=sizeof(Row);h->capacity_rows=capacity;h->pid=getpid();tid=syscall(SYS_gettid);h->writer_tid=tid;h->initialized_ns=now();std::memcpy(h->magic,"VKFLIP01",8);
 }
 bool owned(){if(!h)return false;static thread_local const auto current=uint64_t(syscall(SYS_gettid));if(current==tid)return true;std::atomic_ref(h->foreign_writer).fetch_add(1,std::memory_order_relaxed);return false;}
 void append(uint32_t kind,uint64_t id,uint32_t flags,uint32_t buffer,uint64_t payload=0){if(!h)return;const auto seq=std::atomic_ref(h->arm_seq).load(std::memory_order_acquire);if(!seq||(seq&1))return;
  const auto start=std::atomic_ref(h->start_ns).load(std::memory_order_relaxed),end=std::atomic_ref(h->end_ns).load(std::memory_order_relaxed);if(seq!=std::atomic_ref(h->arm_seq).load(std::memory_order_acquire))return;const auto stamp=now();if(stamp<start||stamp>end)return;
  const auto index=h->used;if(index>=capacity){std::atomic_ref(h->dropped).fetch_add(1,std::memory_order_relaxed);return;}auto& row=rows[index];row.ns=stamp;row.flip_id=id;row.payload=payload;row.kind=kind;row.flags=flags;row.buffer=buffer;row.reserved=0;std::atomic_ref(row.sequence).store(index+1,std::memory_order_release);std::atomic_ref(h->used).store(index+1,std::memory_order_release);
 }
};
inline Writer* writer(){static Writer w;return w.h?&w:nullptr;}
inline thread_local uint64_t current_flip=0,present_flip=0;
inline thread_local uint32_t current_flags=0,current_buffer=0;
struct Scope {
 Writer* w;uint64_t id=0,old_id=current_flip;uint32_t flags,buffer,old_flags=current_flags,old_buffer=current_buffer;bool done=false;
 Scope(bool emu,bool skip,uint32_t buf):w(writer()),flags((emu?uint32_t(emulated):0u)|(skip?uint32_t(skipped):0u)),buffer(buf){if(w&&!w->owned())w=nullptr;if(!w)return;id=++w->next_flip;current_flip=id;current_flags=flags;current_buffer=buf;w->append(begin,id,flags,buffer);}
 void complete(bool unavailable_now,uint64_t path){if(!w)return;done=true;if(flags&emulated)std::atomic_ref(w->h->completed_emu_flips).fetch_add(1,std::memory_order_relaxed);w->append(completed,id,flags|(unavailable_now?uint32_t(unavailable):0u),buffer,path);}
 ~Scope(){if(!w)return;if(!done)w->append(aborted,id,flags,buffer);current_flip=old_id;current_flags=old_flags;current_buffer=old_buffer;}
};
struct PresentBinding {uint64_t old=present_flip;PresentBinding(){present_flip=current_flip;}~PresentBinding(){present_flip=old;}};
inline void record_present(int32_t result,uint32_t image){if(auto* w=writer();w&&w->owned())w->append(present,present_flip,current_flags,image,uint64_t(int64_t(result)));}
}
