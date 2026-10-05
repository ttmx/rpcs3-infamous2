#pragma once

// Bounded observational trace only. No resource pointer is retained/dereferenced.
#include "Utilities/Thread.h"
#include "util/asm.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <new>
#include <type_traits>

namespace rsx::readback_chain_trace
{
inline const char* const path = std::getenv("RPCS3_VK_READBACK_CHAIN_TRACE_PATH");
inline const char* const arm_file = std::getenv("RPCS3_VK_READBACK_CHAIN_TRACE_ARM_FILE");
inline const bool configured = path && *path;
inline constexpr std::uint64_t capacity = 100000;
inline std::atomic<std::uint64_t> serial{1}, arm_calls{0}, armed_ns{0};
inline std::atomic<bool> closed{false};
inline std::uint64_t now()
{
 return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::uint64_t next_id() { return configured ? serial.fetch_add(1, std::memory_order_relaxed) : 0; }
inline thread_local std::uint64_t parent = 0;
template<class T> inline std::uint64_t handle_key(T value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(value);else return static_cast<std::uint64_t>(value);}
struct row
{
 const char* label;
 std::uint64_t begin, end, tid, cookie, parent_cookie, command, generation, access;
 std::uint64_t event_cookie, event, object, image, memory, address, length, aux0, aux1;
};
struct output
{
 std::mutex mutex;
 std::unique_ptr<row[]> captured{new(std::nothrow) row[capacity]};
 std::uint64_t rows=0,dropped=0,filtered=0,late=0,inflight=0;
 bool arm_row=false;
 output(){if(captured)std::memset(captured.get(),0,sizeof(row)*capacity);}
};
inline output& storage(){static output out;return out;}
// Caller owns output mutex. Only post-deadline, with all registered scopes drained.
inline void finalize(output& out)
{
 if(closed.load(std::memory_order_relaxed)||out.inflight)return;
 const auto armed=armed_ns.load(std::memory_order_acquire);
 if(!armed||now()<=armed+5000000000ull)return;
 closed.store(true,std::memory_order_release);
 auto* file=std::fopen(path,"w");if(!file)return;
 std::fputs("label,begin_ns,end_ns,tid,cookie,parent_cookie,command,generation,access,event_cookie,event,object,image,memory,address,length,aux0,aux1\n",file);
 for(std::uint64_t i=0;i<out.rows;++i)
 {
  const auto& x=out.captured[i];
  std::fprintf(file,"%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",x.label,
   static_cast<unsigned long long>(x.begin),static_cast<unsigned long long>(x.end),static_cast<unsigned long long>(x.tid),static_cast<unsigned long long>(x.cookie),static_cast<unsigned long long>(x.parent_cookie),
   static_cast<unsigned long long>(x.command),static_cast<unsigned long long>(x.generation),static_cast<unsigned long long>(x.access),static_cast<unsigned long long>(x.event_cookie),static_cast<unsigned long long>(x.event),
   static_cast<unsigned long long>(x.object),static_cast<unsigned long long>(x.image),static_cast<unsigned long long>(x.memory),static_cast<unsigned long long>(x.address),static_cast<unsigned long long>(x.length),
   static_cast<unsigned long long>(x.aux0),static_cast<unsigned long long>(x.aux1));
 }
 std::fprintf(file,"#closed,%llu,%llu,%s,dropped=%llu,filtered=%llu,late=%llu,inflight=%llu\n",static_cast<unsigned long long>(now()),static_cast<unsigned long long>(out.rows),out.dropped?"capacity":"duration",static_cast<unsigned long long>(out.dropped),static_cast<unsigned long long>(out.filtered),static_cast<unsigned long long>(out.late),static_cast<unsigned long long>(out.inflight));
 std::fclose(file);
}
inline bool active()
{
 if(!configured||closed.load(std::memory_order_relaxed))return false;
 // Allocate and touch all pages before observing the arm marker.
 auto& out=storage();if(!out.captured){closed.store(true,std::memory_order_release);return false;}
 if(armed_ns.load(std::memory_order_acquire))return true;
 if(arm_file&&*arm_file)
 {
  if(arm_calls.fetch_add(1,std::memory_order_relaxed)%256)return false;
  auto* marker=std::fopen(arm_file,"r");if(!marker)return false;std::fclose(marker);
 }
 std::uint64_t expected=0;armed_ns.compare_exchange_strong(expected,now(),std::memory_order_acq_rel);
 return true;
}
inline std::uint64_t register_scope()
{
 if(!active())return 0;
 auto& out=storage();std::lock_guard lock(out.mutex);
 if(closed.load(std::memory_order_relaxed))return 0;
 const auto begin=now();const auto armed=armed_ns.load(std::memory_order_acquire);
 if(begin>armed+5000000000ull){finalize(out);return 0;}
 if(!out.arm_row){row r{};r.label="armed";r.begin=r.end=armed;out.captured[out.rows++]=r;out.arm_row=true;}
 ++out.inflight;return begin;
}
inline void publish(const row& r,bool include)
{
 auto& out=storage();std::lock_guard lock(out.mutex);
 if(!out.inflight)utils::trap();--out.inflight;
 if(!include)++out.filtered;
 else
 {
  if(r.end>armed_ns.load(std::memory_order_acquire)+5000000000ull)++out.late;
  if(out.rows>=capacity)++out.dropped;else out.captured[out.rows++]=r;
 }
 finalize(out);
}
class scope
{
 row r; // Deliberately uninitialized when disabled, avoiding large metadata clears.
 std::uint64_t begin=0, old_parent=0;
 bool nested=false;
public:
 explicit scope(const char* label,bool nest=false)
 {
  begin=register_scope();if(!begin)return;r={};r.label=label;r.begin=begin;r.tid=thread_ctrl::get_tid();r.cookie=next_id();r.parent_cookie=parent;
  if(nest){nested=true;old_parent=parent;parent=r.cookie;}
 }
 scope(const scope&)=delete;
 explicit operator bool() const { return begin!=0; }
 template<class Cmd> void command(const Cmd& cmd)
 {
  if(!begin)return;
  if constexpr(requires{cmd.diagnostic_recording_generation();cmd.diagnostic_command_key();cmd.access_hint;})
  {r.command=cmd.diagnostic_command_key();r.generation=cmd.diagnostic_recording_generation();r.access=static_cast<std::uint64_t>(cmd.access_hint);}
 }
 void packet(std::uint64_t cmd,std::uint64_t generation,std::uint64_t access){if(begin){r.command=cmd;r.generation=generation;r.access=access;}}
 void event(std::uint64_t cookie,const void* object){if(begin){r.event_cookie=cookie;r.event=reinterpret_cast<std::uintptr_t>(object);}}
 template<class Image> void image(const Image* object)
 {
  if(!begin || !object)return;
  r.object=reinterpret_cast<std::uintptr_t>(object);r.memory=reinterpret_cast<std::uintptr_t>(object->memory.get());
  if constexpr(std::is_pointer_v<decltype(object->value)>)r.image=reinterpret_cast<std::uintptr_t>(object->value);
  else r.image=static_cast<std::uint64_t>(object->value);
 }
 void range(std::uint64_t address,std::uint64_t length){if(begin){r.address=address;r.length=length;}}
 void auxiliary(std::uint64_t a,std::uint64_t b=0){if(begin){r.aux0=a;r.aux1=b;}}
 void finish(std::uint64_t minimum_ns=0)
 {
  if(!begin)return;
  r.end=now();if(nested){parent=old_parent;nested=false;}
  publish(r,r.end-r.begin>=minimum_ns);begin=0;
 }
 ~scope(){finish();}
};
}
