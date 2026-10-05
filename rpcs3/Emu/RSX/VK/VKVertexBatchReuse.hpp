#pragma once

// Offline default-off candidate: no shared runtime source edits yet.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>


namespace vk::vertex_batch_reuse
{
inline bool enabled()
{
    static const bool value = [] { const char* p = std::getenv("RPCS3_VK_VERTEX_BATCH_REUSE"); return p && std::strcmp(p, "1") == 0; }();
    return value;
}
inline std::size_t minimum_bytes()
{
    static const std::size_t value = [] {
        const char* p = std::getenv("RPCS3_VK_VERTEX_BATCH_REUSE_MIN_BYTES");
        if(!p||!*p)return std::size_t{0};
        char* end=nullptr;
        const auto parsed=std::strtoull(p,&end,10);
        return end!=p&&!*end&&parsed<=8*1024*1024 ? static_cast<std::size_t>(parsed) : std::size_t{0};
    }();
    return value;
}
struct block
{
    std::uint32_t address{}, length{}, base_offset{}, shape{};
    bool operator==(const block&) const = default;
};
struct key
{
    std::uint32_t command{}, attribute_mask{}, block_count{}, attribute_count{};
    std::array<block,16> blocks{};
    std::array<std::uint32_t,16> attributes{};
    bool operator==(const key& other) const
    {
        if (command != other.command || attribute_mask != other.attribute_mask || block_count != other.block_count || attribute_count != other.attribute_count) return false;
        for (unsigned i=0;i<block_count;i++) if (!(blocks[i] == other.blocks[i])) return false;
        for (unsigned i=0;i<attribute_count;i++) if (attributes[i] != other.attributes[i]) return false;
        return true;
    }
};
struct hash
{
    std::size_t operator()(const key& value) const noexcept
    {
        std::uint64_t result = 0x9e3779b97f4a7c15ull;
        const auto mix=[&](std::uint32_t word) { result ^= word; result *= 0x100000001b3ull; result ^= result >> 29; };
        mix(value.command);mix(value.attribute_mask);mix(value.block_count);mix(value.attribute_count);
        for(unsigned i=0;i<value.block_count;i++) { const auto& b=value.blocks[i];mix(b.address);mix(b.length);mix(b.base_offset);mix(b.shape); }
        for(unsigned i=0;i<value.attribute_count;i++)mix(value.attributes[i]);
        return result;
    }
};
struct result { bool handled{}, reused{}; std::uint64_t absolute_offset{}; };
class cache
{
    static constexpr std::size_t byte_cap = 8*1024*1024;
    static constexpr std::size_t entry_cap = 2048;
    static constexpr std::size_t slot_count = 4096;
    struct entry { key value{}; std::size_t shadow_offset{}, length{}; std::uint64_t gpu_offset{}, epoch{}; };
    std::unique_ptr<entry[]> entries;
    std::unique_ptr<std::byte[]> shadow;
    std::size_t used{}, count{};
    std::uint64_t frame{},gpu_uid{},upload_uid{},epoch{},last_end{};
    bool have_scope{},have_allocation{},processing{},pending_clear{};

    static bool valid(const key& value, std::size_t length)
    {
        if(value.block_count<=1||value.block_count>16||value.attribute_count>16||!length||length>byte_cap)return false;
        std::size_t total=0;
        for(unsigned i=0;i<value.block_count;i++) {
            if(!value.blocks[i].length||value.blocks[i].length>length-total)return false;
            total+=value.blocks[i].length;
        }
        return total==length;
    }
    void invalidate()
    {
        ++epoch;
        if(processing) { pending_clear=true; return; }
        used=0;count=0;
    }
    bool storage()
    {
        if(shadow&&entries)return true;
        shadow.reset(new(std::nothrow) std::byte[byte_cap]);
        entries.reset(new(std::nothrow) entry[slot_count]);
        if(shadow&&entries)return true;
        shadow.reset();entries.reset();return false;
    }
    entry* slot(const key& value)
    {
        if(!entries)return nullptr;
        std::size_t index=hash{}(value)&(slot_count-1);
        for(std::size_t checked=0;checked<slot_count;checked++,index=(index+1)&(slot_count-1)) {
            auto& item=entries[index];
            if(item.epoch!=epoch||item.value==value)return &item;
        }
        return nullptr;
    }
    void remember(const key& value,const void* copied,std::size_t length,std::uint64_t offset)
    {
        if(processing||pending_clear||!have_scope||!copied||!valid(value,length)||!storage())return;
        auto* item=slot(value);
        if(!item)return;
        if(item->epoch==epoch) {
            if(item->length!=length)return;
            std::memcpy(shadow.get()+item->shadow_offset,copied,length);item->gpu_offset=offset;return;
        }
        if(count>=entry_cap||length>byte_cap-used)return;
        *item=entry{value,used,length,offset,epoch};
        std::memcpy(shadow.get()+used,copied,length);used+=length;count++;
    }
public:
    cache()=default;
    cache(const cache&)=delete;
    cache& operator=(const cache&)=delete;
    void scope(std::uint64_t current_frame,std::uint64_t current_gpu,std::uint64_t current_upload)
    {
        if(!have_scope||frame!=current_frame||gpu_uid!=current_gpu||upload_uid!=current_upload) {
            invalidate();frame=current_frame;gpu_uid=current_gpu;upload_uid=current_upload;have_scope=true;have_allocation=false;
        }
    }
    // Call after EVERY attribute allocation, persistent or volatile, before any write.
    // Length is the allocator's aligned reserved length, not just the payload bytes.
    void allocation(std::uint64_t current_frame,std::uint64_t current_gpu,std::uint64_t current_upload,
        std::uint64_t offset,std::uint64_t reserved_length)
    {
        scope(current_frame,current_gpu,current_upload);
        if(have_allocation&&offset<last_end)invalidate();
        last_end=offset+reserved_length;have_allocation=true;
    }
    std::uint64_t generation() const { return epoch; }
    // Caller performs original stock copy for a miss, then records its actual mapped bytes.
    void stock_copy(const key& value,const void* copied,std::size_t length,std::uint64_t absolute_offset)
    {
        if(valid(value,length))
            remember(value,copied,length,absolute_offset);
    }
    template<class SpanCopy,class ScopeCheck>
    result attempt(const key& value,void* fresh,std::size_t length,std::uint64_t absolute_offset,
        SpanCopy&& span_copy,ScopeCheck&& scope_check)
    {
        if(processing||pending_clear||!fresh||!have_scope||!valid(value,length))return {false,false,absolute_offset};
        const auto* found=slot(value);
        if(!found||found->epoch!=epoch||found->length!=length)return {false,false,absolute_offset};
        const auto saved_shadow_offset=found->shadow_offset;
        const auto saved_gpu_offset=found->gpu_offset;
        const auto entry_epoch=epoch;
        const auto* expected=shadow.get()+saved_shadow_offset;
        auto* destination=static_cast<std::byte*>(fresh);
        std::size_t prefix=0;
        bool copying=false;
        processing=true;
        struct release {
            cache& owner;
            ~release(){owner.processing=false;if(owner.pending_clear){owner.used=0;owner.count=0;owner.pending_clear=false;}}
        } guard{*this};
        for(unsigned i=0;i<value.block_count;i++) {
            const auto span_length=value.blocks[i].length;
            // The callback holds the original per-span lock across comparison and copy.
            // expected==nullptr means ordinary copy; on mismatch it copies this span before releasing the SAME lock.
            const bool equal=span_copy(i,copying?nullptr:expected+prefix,destination+prefix,span_length);
            if(!copying&&!equal) {
                if(prefix)std::memcpy(destination,expected,prefix);
                copying=true;
            }
            prefix+=span_length;
        }
        scope_check(); // Recheck after faultable reads/waits; it may invalidate scope/rollover epoch.
        if(!copying&&epoch==entry_epoch&&!pending_clear)return {true,true,saved_gpu_offset};
        if(!copying)std::memcpy(destination,expected,length);
        // All fallback bytes already preserve each original span snapshot. Caller may remember after guard exits.
        return {true,false,absolute_offset};
    }
};
inline cache& instance(){static cache value;return value;}
}
