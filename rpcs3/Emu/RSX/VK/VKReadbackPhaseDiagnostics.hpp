#pragma once

// Default-off bounded GPU command phase spans. No polling/waiting is added;
// collect is called only after the existing exact readback event has completed.
#include "vkutils/device.h"
#include "vkutils/commands.h"
#include "../Common/readback_chain_diagnostics.hpp"
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace vk
{
class readback_phase_sample
{
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    double m_period = 0;
    u32 m_valid_bits = 0;
    u64 m_command = 0, m_generation = 0, m_access = 0, m_cookie = 0;
    u32 m_address = 0, m_length = 0;
    std::atomic<bool> m_collected{false};

public:
    readback_phase_sample(const render_device& dev, const command_buffer& cmd,
        u64 cookie, u32 address, u32 length)
        : m_device(dev), m_period(dev.gpu().get_limits().timestampPeriod),
          m_command(cmd.diagnostic_command_key()), m_generation(cmd.diagnostic_recording_generation()),
          m_access(static_cast<u64>(cmd.access_hint)), m_cookie(cookie), m_address(address), m_length(length)
    {
        u32 count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu(), &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu(), &count, families.data());
        const auto family = dev.get_graphics_queue_family();
        if (family >= count) return;
        m_valid_bits = families[family].timestampValidBits;
        if (!m_valid_bits || m_valid_bits > 64 || !(m_period > 0)) return;
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 4;
        if (vkCreateQueryPool(m_device, &info, nullptr, &m_pool) != VK_SUCCESS)
            m_pool = VK_NULL_HANDLE;
    }

    ~readback_phase_sample()
    {
        // Every recorded object is retired through the existing GPU GC, or
        // destroyed after a failed constructor before any GPU command used it.
        if (m_pool) vkDestroyQueryPool(m_device, m_pool, nullptr);
    }

    static std::unique_ptr<readback_phase_sample> create(const render_device& dev,
        const command_buffer& cmd, bool armed, u64 cookie, u32 address, u32 length)
    {
        static const bool enabled = []()
        {
            const auto* value = std::getenv("RPCS3_VK_READBACK_PHASE_TRACE");
            return value && std::strcmp(value, "1") == 0;
        }();
        static std::atomic<u32> attempted{0};
        if (!enabled || !armed || !cookie || cmd.access_hint != command_buffer::flush_only ||
            (address != 0x37400b80 && address != 0x37784b80) || length != 3686400)
            return {};
        auto count = attempted.load(std::memory_order_relaxed);
        do
        {
            if (count >= 32) return {};
        } while (!attempted.compare_exchange_weak(count, count + 1,
            std::memory_order_relaxed, std::memory_order_relaxed));
        auto sample = std::make_unique<readback_phase_sample>(dev, cmd, cookie, address, length);
        if (!sample->m_pool) return {};
        vkCmdResetQueryPool(cmd, sample->m_pool, 0, 4);
        return sample;
    }

    void stamp(const command_buffer& cmd, u32 index, VkPipelineStageFlagBits stage) const
    {
        vkCmdWriteTimestamp(cmd, stage, m_pool, index);
    }

    void collect_after_original_wait(const void* completed_event)
    {
        if (m_collected.exchange(true, std::memory_order_relaxed)) return;
        u64 data[8]{}; // tick,availability for each of four queries
        const auto status = vkGetQueryPoolResults(m_device, m_pool, 0, 4,
            sizeof(data), data, 2 * sizeof(u64),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        u64 available = 0;
        for (u32 i = 0; i < 4; ++i) if (data[i * 2 + 1]) available |= u64{1} << i;
        auto publish = [&](const char* label, u64 a, u64 b)
        {
            rsx::readback_chain_trace::scope row(label);
            row.packet(m_command, m_generation, m_access);
            row.event(m_cookie, completed_event); row.range(m_address, m_length);
            row.auxiliary(a, b);
        };
        if (status != VK_SUCCESS || available != 15)
        {
            publish("readback_gpu_query_unavailable", static_cast<u64>(static_cast<s64>(status)), available);
            return;
        }
        publish("readback_gpu_query_properties", std::bit_cast<u64>(m_period), m_valid_bits);
        publish("readback_gpu_image_copy_ticks", data[0], data[2]);
        publish("readback_gpu_shuffle_ticks", data[2], data[4]);
        publish("readback_gpu_final_copy_ticks", data[4], data[6]);
    }
};
}
