#pragma once

// Isolated diagnostic: same raw device scratch feeds an out-of-place integer
// bswap and the unchanged stock in-place bswap/copy. Stock VM retirement stays.
#include "VKCompute.h"
#include "VKHelpers.h"
#include "../Common/readback_chain_diagnostics.hpp"
#include "VKDMA.h"
#include "VKReadbackOOPControl.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace vk
{
struct cs_readback_swap32_copy : cs_shuffle_base
{
    const vk::buffer* m_source = nullptr;
    u32 m_source_offset = 0;

    cs_readback_swap32_copy()
    {
        ssbo_count = 2;
        method_declarations = "layout(set=0,binding=1,std430) readonly buffer readback_input { uint source_data[]; };\n";
        work_kernel = "\t\tvalue = source_data[index];\n\t\tdata[index] = %f(value);\n";
        cs_shuffle_base::build("bswap_u32");
    }

    void bind_resources(const vk::command_buffer& cmd) override
    {
        cs_shuffle_base::bind_resources(cmd);
        m_program->bind_uniform({*m_source, m_source_offset, m_data_length}, 0, 1);
    }

    void run(const vk::command_buffer& cmd, const vk::buffer* destination, u32 destination_offset,
        const vk::buffer* source, u32 source_offset, u32 length)
    {
        m_source = source;
        m_source_offset = source_offset;
        cs_shuffle_base::run(cmd, destination, length, destination_offset);
    }
};

class readback_oop_shadow
{
    std::unique_ptr<vk::buffer> m_target;
    u32 m_offset = 0, m_length = 0, m_address = 0;
    u64 m_cookie = 0, m_command = 0, m_generation = 0, m_access = 0;
    const void* m_stock_memory = nullptr;
    u64 m_stock_buffer = 0;
    std::atomic<bool> m_collected{false};
    static constexpr u32 guard_bytes = 64;
    static constexpr u32 guard_word = 0xa57ce319;
    inline static std::atomic<u32> s_attempted{0};

    explicit readback_oop_shadow(std::unique_ptr<vk::buffer> target,
        const command_buffer& cmd, const vk::buffer* stock, u64 cookie, u32 address, u32 offset, u32 length)
        : m_target(std::move(target)), m_offset(offset), m_length(length), m_address(address),
          m_cookie(cookie), m_command(cmd.diagnostic_command_key()),
          m_generation(cmd.diagnostic_recording_generation()), m_access(static_cast<u64>(cmd.access_hint)),
          m_stock_memory(stock->memory.get()), m_stock_buffer(rsx::frozen_handle_key(stock->value)) {}

public:
    static bool enabled()
    {
        static const bool value = []
        {
            const auto* text = std::getenv("RPCS3_VK_READBACK_OOP_SHADOW");
            return text && std::strcmp(text, "1") == 0;
        }();
        return value;
    }

    static bool available() { return enabled() && s_attempted.load(std::memory_order_relaxed) < 32; }

    static std::unique_ptr<readback_oop_shadow> record(const render_device& dev,
        const command_buffer& cmd, const vk::buffer* scratch, u32 scratch_offset,
        const vk::buffer* stock, u64 cookie, u32 address, u32 destination_offset, u32 length)
    {
        if (!available() || cmd.access_hint != command_buffer::flush_only || !cookie ||
            (address != 0x37400b80 && address != 0x37784b80) || length != 3686400 ||
            !scratch || !(scratch->info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !stock || !stock->memory ||
            dynamic_cast<vk::memory_block_host*>(stock->memory.get()) ||
            static_cast<u64>(destination_offset) + length > stock->size()) return {};
        const auto& limits = dev.gpu().get_limits();
        const auto alignment = limits.minStorageBufferOffsetAlignment;
        if (!alignment || scratch_offset % alignment || destination_offset % alignment ||
            length > limits.maxStorageBufferRange || destination_offset < guard_bytes ||
            static_cast<u64>(scratch_offset) + length > scratch->size() ||
            static_cast<u64>(destination_offset) + length + guard_bytes > 8 * 1024 * 1024) return {};
        const auto stock_type = stock->memory->diagnostic_memory_type();
        const auto& stock_properties = dev.gpu().get_memory_properties();
        if (stock_type >= stock_properties.memoryTypeCount ||
            (stock_properties.memoryTypes[stock_type].propertyFlags & 14u) != 14u) return {};
        auto* kernel = vk::get_compute_task<cs_readback_swap32_copy>();
        const u64 quantum = static_cast<u64>(kernel->optimal_group_size) * kernel->kernel_size * 4;
        if (!quantum || length % quantum || length / quantum > kernel->max_invocations_x) return {};
        auto count = s_attempted.load(std::memory_order_relaxed);
        do { if (count >= 32) return {}; }
        while (!s_attempted.compare_exchange_weak(count, count + 1,
            std::memory_order_relaxed, std::memory_order_relaxed));
        const auto extent = utils::align(static_cast<u64>(destination_offset) + length + guard_bytes, 256);
        auto target = std::make_unique<vk::buffer>(dev, extent,
            dev.get_memory_mapping().host_visible_coherent,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_BUFFER_CREATE_ALLOW_NULL_RPCS3 | VK_BUFFER_CREATE_IGNORE_VMEM_PRESSURE_RPCS3,
            VMM_ALLOCATION_POOL_UNDEFINED);
        if (!target->value || !target->memory) return {};
        if (dynamic_cast<vk::memory_block_host*>(target->memory.get())) return {};
        const auto type = target->memory->diagnostic_memory_type();
        const auto& properties = dev.gpu().get_memory_properties();
        if (type >= properties.memoryTypeCount ||
            (properties.memoryTypes[type].propertyFlags & 14u) != 14u) return {};
        auto result = std::unique_ptr<readback_oop_shadow>(new readback_oop_shadow(
            std::move(target), cmd, stock, cookie, address, destination_offset, length));
        vkCmdFillBuffer(cmd, result->m_target->value, destination_offset - guard_bytes, guard_bytes, guard_word);
        vkCmdFillBuffer(cmd, result->m_target->value, destination_offset + length, guard_bytes, guard_word);
        vkCmdFillBuffer(cmd, result->m_target->value, destination_offset, length, 0xcd79f05b);
        vk::insert_buffer_memory_barrier(cmd, result->m_target->value, destination_offset, length,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        kernel->run(cmd, result->m_target.get(), destination_offset, scratch, scratch_offset, length);
        // Protect raw scratch against the following stock in-place writer.
        vk::insert_buffer_memory_barrier(cmd, scratch->value, scratch_offset, length,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        // The target is separate normal coherent RAM. Include both sentinels.
        vk::insert_buffer_memory_barrier(cmd, result->m_target->value,
            destination_offset - guard_bytes, length + 2 * guard_bytes,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        rsx::readback_chain_trace::scope row("readback_oop_shadow_record");
        row.command(cmd); row.event(cookie, nullptr); row.image(result->m_target.get()); row.range(address, length);
        row.auxiliary(destination_offset, quantum);
        return result;
    }

    struct observer_binding
    {
        readback_oop_shadow* shadow;
        const void* completed_event;
        dma_source_observer observer()
        {
            return {this, [](void* self, const void* source, const vk::buffer* stock, u32 offset, u32 address, u32 length)
            {
                const auto& binding = *static_cast<observer_binding*>(self);
                binding.shadow->compare_completed_source(binding.completed_event, source, stock, offset, address, length);
            }};
        }
    };

    observer_binding bind_observer(const void* completed_event) { return {this, completed_event}; }

    void compare_completed_source(const void* completed_event, const void* expected, const vk::buffer* stock,
        u32 offset, u32 address, u32 length)
    {
        if (m_collected.exchange(true, std::memory_order_relaxed)) return;
        const bool exact_source = expected && stock && stock->memory.get() == m_stock_memory &&
            rsx::frozen_handle_key(stock->value) == m_stock_buffer && offset == m_offset &&
            address == m_address && length == m_length;
        rsx::readback_chain_trace::scope source_row("readback_oop_shadow_stock_source");
        source_row.packet(m_command, m_generation, m_access); source_row.event(m_cookie, completed_event);
        source_row.image(stock); source_row.range(address, length); source_row.auxiliary(offset, exact_source ? 1 : 0);
        source_row.finish();
        auto* mapped = exact_source ? static_cast<const std::byte*>(m_target->map(0, m_target->size())) : nullptr;
        if (!mapped)
        {
            rsx::readback_chain_trace::scope row("readback_oop_shadow_unavailable");
            row.packet(m_command, m_generation, m_access); row.event(m_cookie, completed_event);
            row.image(m_target.get()); row.range(address, length); row.auxiliary(exact_source ? 1 : 2, 0);
            return;
        }
        const bool equal = std::memcmp(expected, mapped + m_offset, m_length) == 0;
        bool guards_equal = true;
        for (u32 i = 0; i < guard_bytes; i += 4)
        {
            u32 before = 0, after = 0;
            std::memcpy(&before, mapped + m_offset - guard_bytes + i, 4);
            std::memcpy(&after, mapped + m_offset + m_length + i, 4);
            guards_equal &= before == guard_word && after == guard_word;
        }
        // Diagnostic only: prove real source includes nonzero and non-palindromic
        // swap32 words, rather than accepting only an all-zero output fixture.
        u64 nonzero = 0, asymmetric = 0;
        for (u32 i = 0; i < m_length; i += 4)
        {
            u32 word = 0;
            std::memcpy(&word, static_cast<const std::byte*>(expected) + i, 4);
            nonzero += word != 0;
            asymmetric += std::byteswap(word) != word;
        }
        m_target->unmap();
        rsx::readback_chain_trace::scope evidence("readback_oop_shadow_evidence");
        evidence.packet(m_command, m_generation, m_access); evidence.event(m_cookie, completed_event);
        evidence.image(m_target.get()); evidence.range(address, length); evidence.auxiliary(nonzero, asymmetric);
        evidence.finish();
        rsx::readback_chain_trace::scope row("readback_oop_shadow_compare");
        row.packet(m_command, m_generation, m_access); row.event(m_cookie, completed_event);
        row.image(m_target.get()); row.range(address, length); row.auxiliary(equal ? 1 : 0, guards_equal ? 1 : 0);
    }
};
// Guarded behavior candidate: keep original image-to-device-scratch operation,
// fuse integer shuffle and final copy into a separate actual staging write.
class readback_oop_fusion
{
public:
    static bool run(const render_device& dev, const command_buffer& cmd,
        const vk::buffer* scratch, u32 source_offset, const vk::buffer* destination,
        u32 destination_offset, u32 address, u32 length, u64 transfer_cookie)
    {
        // Diagnostic shadow always keeps the stock authoritative output route.
        if (!readback_oop_enabled() || readback_oop_shadow::enabled() ||
            cmd.access_hint != command_buffer::flush_only ||
            (address != 0x37400b80 && address != 0x37784b80) || length != 3686400 ||
            !scratch || !scratch->memory || !destination || !destination->memory ||
            scratch->value == destination->value || scratch->memory.get() == destination->memory.get() ||
            !(scratch->info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !(destination->info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            dynamic_cast<vk::memory_block_host*>(destination->memory.get())) return false;
        const auto& limits = dev.gpu().get_limits();
        const auto alignment = limits.minStorageBufferOffsetAlignment;
        if (!alignment || source_offset % alignment || destination_offset % alignment ||
            length > limits.maxStorageBufferRange ||
            static_cast<u64>(source_offset) + length > scratch->size() ||
            static_cast<u64>(destination_offset) + length > destination->size()) return false;
        const auto type = destination->memory->diagnostic_memory_type();
        const auto& properties = dev.gpu().get_memory_properties();
        if (type >= properties.memoryTypeCount ||
            (properties.memoryTypes[type].propertyFlags & 14u) != 14u) return false;
        auto* kernel = vk::get_compute_task<cs_readback_swap32_copy>();
        const u64 quantum = static_cast<u64>(kernel->optimal_group_size) * kernel->kernel_size * 4;
        if (!quantum || length % quantum || length / quantum > kernel->max_invocations_x) return false;
        // All unsupported cases above are untouched: stock shuffle+copy follows.
        // No source pointer/lease/protection lifetime or CPU authority changes.
        // The previous producer/consumer may have used transfer or compute.
        // A transfer-only destination reuse dependency is insufficient now.
        vk::insert_buffer_memory_barrier(cmd, destination->value, destination_offset, length,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT,
            VK_ACCESS_SHADER_WRITE_BIT);
        kernel->run(cmd, destination, destination_offset, scratch, source_offset, length);
        vk::insert_buffer_memory_barrier(cmd, destination->value, destination_offset, length,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        static std::atomic<bool> announced{false};
        if (!announced.exchange(true, std::memory_order_relaxed))
            rsx_log.notice("Readback OOP fusion activated: address=0x%x bytes=%u offset=%u memory_type=%u flags=0x%x usage=0x%x quantum=%u",
                address, length, destination_offset, type, properties.memoryTypes[type].propertyFlags, destination->info.usage, static_cast<u32>(quantum));
        rsx::readback_chain_trace::scope row("readback_oop_fused");
        row.command(cmd); row.event(transfer_cookie, nullptr); row.image(destination); row.range(address, length); row.auxiliary(destination_offset, quantum);
        return true;
    }
};
}
