#pragma once
// Bounded default-off caller/phase telemetry; no content reads or validity decisions.
// Call sites are on the RSX recording thread. TLS tracks nesting; the bounded writer
// is synchronized, and never retains source/image pointers or alters upload lifetime.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>

namespace vk::rtt_load_diagnostics
{
inline bool enabled()
{
    static const bool value=[]{const char* flag=std::getenv("RPCS3_VK_RTT_LOAD_TRACE");const char* path=std::getenv("RPCS3_VK_RTT_LOAD_TRACE_PATH");return flag&&std::strcmp(flag,"1")==0&&path&&*path;}();return value;
}
inline std::uint64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
enum field : unsigned {image_uid,address,length,width,height,pitch,gcm_format,host_format,aspect,state_flags,msaa_flags,old_contents,last_use_tag,cache_locked,cache_timestamp,access,blit_active,integrity_reload,discard_allowed,x1,y1,x2,y2,tiled,swizzled,spp,scale,source_gpu,require_upload,require_swap,require_deswizzle,depth_process,passthrough,upload_buffer_uid,upload_offset,field_count};
inline constexpr const char* names="image_uid,address,length,width,height,pitch,gcm_format,host_format,aspect,state_flags,msaa_flags,old_contents,last_use_tag,cache_locked,cache_timestamp,access,blit_active,integrity_reload,discard_allowed,x1,y1,x2,y2,tiled,swizzled,spp,scale,source_gpu,require_upload,require_swap,require_deswizzle,depth_process,passthrough,upload_buffer_uid,upload_offset";
struct row {const char* kind;std::uint64_t begin,end,frame,id,parent;std::array<std::uint64_t,field_count> values;};
inline thread_local const row* current_context=nullptr;
inline std::atomic<std::uint64_t> ids{0};
class output
{
    static constexpr std::size_t cap=8192;
    std::mutex mutex;std::unique_ptr<row[]> rows;std::size_t count{};std::uint64_t first_frame{},drops{};bool have_frame{},stopped{};FILE* file{};
    void finish(const char* reason)
    {
        if(stopped)return;
        stopped=true;
        if(!file)return;
        for(std::size_t i=0;i<count;i++){
            const auto& r=rows[i];std::fprintf(file,"%s,%llu,%llu,%llu,%llu,%llu",r.kind,static_cast<unsigned long long>(r.begin),static_cast<unsigned long long>(r.end),static_cast<unsigned long long>(r.frame),static_cast<unsigned long long>(r.id),static_cast<unsigned long long>(r.parent));
            for(unsigned f=0;f<field_count;f++)std::fprintf(file,",%llu",static_cast<unsigned long long>(r.values[f]));
            std::fputc('\n',file);
        }
        std::fprintf(file,"#stop,%s,%llu,rows=%zu,dropped=%llu\n",reason,static_cast<unsigned long long>(now_ns()),count,static_cast<unsigned long long>(drops));std::fflush(file);
    }
public:
    output()
    {
        rows.reset(new(std::nothrow)row[cap]);
        if(!rows){stopped=true;return;}
        file=std::fopen(std::getenv("RPCS3_VK_RTT_LOAD_TRACE_PATH"),"w");
        if(!file){stopped=true;return;}
        std::fprintf(file,"kind,begin_ns,end_ns,frame,id,parent_id,%s\n#armed,%llu\n",names,static_cast<unsigned long long>(now_ns()));std::fflush(file);
    }
    ~output(){std::lock_guard lock(mutex);finish("destructor");if(file){std::fprintf(file,"#final_dropped,%llu\n",static_cast<unsigned long long>(drops));std::fclose(file);}}
    bool active(std::uint64_t frame)
    {
        std::lock_guard lock(mutex);if(stopped)return false;
        if(!have_frame){first_frame=frame;have_frame=true;}
        if(frame>first_frame&&frame-first_frame>32){finish("frame_cap");return false;}
        if(count>=cap){drops++;finish("row_cap");return false;}
        return true;
    }
    void append(const row& value)
    {
        std::lock_guard lock(mutex);if(stopped){drops++;return;}
        if(count>=cap){drops++;finish("row_cap");return;}rows[count++]=value;
    }
};
inline output& writer(){static output value;return value;}
inline bool active(std::uint64_t frame)
{
    if(!enabled())return false;
    static const std::string arm=[] {const char* p=std::getenv("RPCS3_VK_RTT_LOAD_TRACE_ARM_FILE");return p?std::string(p):std::string{};}();
    if(!arm.empty()){
        static std::atomic<bool> armed{false};static std::atomic<unsigned> polls{0};
        if(!armed.load(std::memory_order_acquire)){
            if(polls.fetch_add(1,std::memory_order_relaxed)&15)return false;
            std::error_code error;if(!std::filesystem::exists(arm,error)||error)return false;armed.store(true,std::memory_order_release);
        }
    }
    return writer().active(frame);
}
class span
{
    // Deliberately uninitialized: disabled construction must not clear row metadata.
    row value;bool observing=false,context=false;const row* previous=nullptr;
public:
    span()=default;span(const span&)=delete;span& operator=(const span&)=delete;
    void begin(const char* kind,std::uint64_t frame,bool become_context=false)
    {
        if(!active(frame))return;
        value.values.fill(0);value.kind=kind;value.frame=frame;value.id=ids.fetch_add(1,std::memory_order_relaxed)+1;
        value.parent=current_context?current_context->id:0;
        if(current_context)value.values=current_context->values;
        previous=current_context;context=become_context;if(context)current_context=&value;
        value.begin=now_ns();observing=true;
    }
    explicit operator bool() const {return observing;}
    void set(field f,std::uint64_t v){if(observing)value.values[f]=v;}
    void end()
    {
        if(!observing)return;
        value.end=now_ns();
        if(context)current_context=previous;
        observing=false;writer().append(value);
    }
    ~span(){end();}
};
}
