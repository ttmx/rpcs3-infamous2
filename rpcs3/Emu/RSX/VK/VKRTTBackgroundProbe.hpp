#pragma once
// Diagnostic only: observes post-stockcopy DMA bytes, owns all retained bytes.
// GPU writer quiescence is UNKNOWN: observed equality is not reuse permission.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace vk::rtt_background_probe
{
using u64=std::uint64_t;
inline u64 now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline bool enabled()
{
 static const bool yes=[]{const char* flag=std::getenv("RPCS3_VK_RTT_BACKGROUND_PROBE");const char* path=std::getenv("RPCS3_VK_RTT_BACKGROUND_PROBE_PATH");return flag&&std::strcmp(flag,"1")==0&&path&&*path;}();return yes;
}
struct key
{
 u64 uid,address,width,height,pitch,gcm_format,host_format,aspect,x1,y1,x2,y2;
 bool operator==(const key&)const=default;
};
struct event{u64 mono_ns,frame,uid,address,bytes,repeat,equal,changed_bytes,flags,wall_ns;};
class collector
{
 static constexpr std::size_t retained_cap=4*1024*1024,sample_cap=4*1024*1024,event_cap=128;
 struct entry{key identity{};std::unique_ptr<unsigned char[]> bytes;std::size_t size{};bool equal_sample{},different_sample{};};
 std::mutex mutex;std::array<entry,8> entries;std::array<event,event_cap> events;
 std::unique_ptr<unsigned char[]> scratch;std::size_t scratch_size{};
 std::size_t retained{},samples{},count{};u64 first_frame{},skipped{},allocation_failed{},dropped{},attempts{},last_flags{},unknown_flags{},uncached_flags{},geometry_skips{};bool started{},stopped{};
 FILE* output{};FILE* sample_file{};
 void finish(const char* reason)
 {
  if(stopped)return;
  stopped=true;
  if(!output)return;
  for(std::size_t i=0;i<count;i++){
   const auto& e=events[i];std::fprintf(output,"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",static_cast<unsigned long long>(e.mono_ns),static_cast<unsigned long long>(e.frame),static_cast<unsigned long long>(e.uid),static_cast<unsigned long long>(e.address),static_cast<unsigned long long>(e.bytes),static_cast<unsigned long long>(e.repeat),static_cast<unsigned long long>(e.equal),static_cast<unsigned long long>(e.changed_bytes),static_cast<unsigned long long>(e.flags),static_cast<unsigned long long>(e.wall_ns));
  }
  std::fprintf(output,"#stop,%s,%llu,events=%zu,retained=%zu,samples=%zu,skipped=%llu,allocation_failed=%llu,dropped=%llu,attempts=%llu,last_flags=%llu,unknown_flags=%llu,uncached_flags=%llu,geometry_skips=%llu\n",reason,static_cast<unsigned long long>(now_ns()),count,retained,samples,static_cast<unsigned long long>(skipped),static_cast<unsigned long long>(allocation_failed),static_cast<unsigned long long>(dropped),static_cast<unsigned long long>(attempts),static_cast<unsigned long long>(last_flags),static_cast<unsigned long long>(unknown_flags),static_cast<unsigned long long>(uncached_flags),static_cast<unsigned long long>(geometry_skips));std::fflush(output);if(sample_file)std::fflush(sample_file);
 }
 void complete_attempt(){if(attempts>=32)finish("attempt_cap");}
 static void pack(unsigned char* dst,const unsigned char* src,const key& k)
 {
  const auto width=(k.width-k.x2)*4;
  for(u64 y=0;y<k.height;y++){std::memcpy(dst,src+y*k.pitch+k.x2*4,width);dst+=width;}
 }
 void sample(entry& old,const unsigned char* packed,bool equal)
 {
  bool& taken=equal?old.equal_sample:old.different_sample;
  const std::size_t bytes=old.size;
  constexpr std::size_t header_size=16*sizeof(u64);
  if(taken||bytes>(sample_cap-header_size)/2||samples>sample_cap-header_size-2*bytes)return;
  if(!sample_file){const char* path=std::getenv("RPCS3_VK_RTT_BACKGROUND_PROBE_SAMPLES");if(!path||!*path)return;sample_file=std::fopen(path,"wb");if(!sample_file)return;}
  const auto& k=old.identity;const std::array<u64,16> header{0x5254544247505242ull,1,k.uid,k.address,k.width,k.height,k.pitch,k.gcm_format,k.host_format,k.aspect,k.x1,k.y1,k.x2,k.y2,bytes,equal?1ull:0ull};
  const bool written=std::fwrite(header.data(),sizeof(u64),header.size(),sample_file)==header.size()&&std::fwrite(old.bytes.get(),1,bytes,sample_file)==bytes&&std::fwrite(packed,1,bytes,sample_file)==bytes;
  if(written){samples+=header_size+2*bytes;taken=true;}else{allocation_failed++;}
 }
public:
 collector()
 {
  output=std::fopen(std::getenv("RPCS3_VK_RTT_BACKGROUND_PROBE_PATH"),"w");
  if(!output){stopped=true;return;}
  std::fputs("mono_ns,frame,image_uid,address,checked_bytes,repeat,equal,changed_bytes,memory_flags,probe_wall_ns\n#writer_quiescence,unknown;observed_bytes_only_not_reuse_permission\n",output);std::fprintf(output,"#armed,%llu\n",static_cast<unsigned long long>(now_ns()));std::fflush(output);
 }
 ~collector(){std::lock_guard lock(mutex);finish("destructor");if(output)std::fclose(output);if(sample_file)std::fclose(sample_file);}
 void observe(const key& k,const void* stock_dst,u64 frame,u64 flags)
 {
  const u64 begin=now_ns();std::lock_guard lock(mutex);if(stopped)return;
  if(!started){started=true;first_frame=frame;}
  if(frame<first_frame||frame-first_frame>=16){finish("frame_cap");return;}
  if(count==event_cap){dropped++;finish("event_cap");return;}
  attempts++;last_flags=flags;
  if((flags&14)!=14){skipped++;if(!flags)unknown_flags++;else uncached_flags++;complete_attempt();return;}
  // Restrict to the measured right-band case; no invented format conversions.
  if(!stock_dst||k.width!=1280||k.height!=720||k.pitch!=5120||k.x1!=0||k.y1!=0||k.x2!=1024||k.y2!=720||k.gcm_format!=133||k.host_format!=44||k.aspect!=1){skipped++;geometry_skips++;complete_attempt();return;}
  constexpr std::size_t bytes=256*720*4;
  entry* old=nullptr;entry* free=nullptr;
  for(auto& e:entries){if(e.bytes&&e.identity==k){old=&e;break;}if(!e.bytes&&!free)free=&e;}
  const auto* src=static_cast<const unsigned char*>(stock_dst);u64 changed=0;bool repeat=old!=nullptr;
  if(!scratch){scratch.reset(new(std::nothrow)unsigned char[bytes]);if(!scratch){allocation_failed++;complete_attempt();return;}scratch_size=bytes;}
  // Copy the strided observed bytes once; later equality/sample checks use owned bytes.
  pack(scratch.get(),src,k);
  if(old){
   if(std::memcmp(old->bytes.get(),scratch.get(),bytes)!=0)for(std::size_t x=0;x<bytes;x++)changed+=old->bytes[x]!=scratch[x];
   // Capture pair before replacing the old exact shadow. No live mapped pointer retained.
   sample(*old,scratch.get(),changed==0);std::memcpy(old->bytes.get(),scratch.get(),bytes);
  }else{
   if(!free||retained+scratch_size>retained_cap-bytes){skipped++;complete_attempt();return;}
   auto saved=std::unique_ptr<unsigned char[]>(new(std::nothrow)unsigned char[bytes]);if(!saved){allocation_failed++;complete_attempt();return;}
   std::memcpy(saved.get(),scratch.get(),bytes);free->identity=k;free->bytes=std::move(saved);free->size=bytes;retained+=bytes;
  }
  events[count++]={begin,frame,k.uid,k.address,bytes,repeat?1ull:0ull,repeat&&changed==0?1ull:0ull,changed,flags,now_ns()-begin};
  complete_attempt();
 }
};
inline void observe(const key& k,const void* dst,u64 frame,u64 flags){if(!enabled())return;static collector output;output.observe(k,dst,frame,flags);}
}
