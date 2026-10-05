#pragma once

#include "VKGSRenderTypes.hpp"
#include "VKBoundedDrawAttribution.hpp"
#include "VKHelpers.h"
#include "vkutils/device.h"
#include "Utilities/File.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <vector>

namespace vk
{
	// Diagnostic only: primary graphics command-buffer spans, not GPU active time.
	class timestamp_diagnostics
	{
		static constexpr u32 slot_count = 1024;
		static constexpr u32 invalid_slot = ~0u;
		struct sample
		{
			command_buffer_chunk* commands = nullptr;
			u64 generation = 0, frame = 0, sequence = 0;
			u64 record_begin = 0, record_end = 0, submit_begin = 0, submit_end = 0;
			bool submitted = false, external_fence = false;
			const char* reason = "unspecified";
		};
		VkDevice m_device = VK_NULL_HANDLE;
		VkQueryPool m_pool = VK_NULL_HANDLE;
		double m_period = 0;
		u32 m_valid_bits = 0, m_active = invalid_slot;
		u64 m_frame = 0, m_sequence = 0, m_skipped = 0;
		std::array<sample, slot_count> m_samples{};
		std::deque<u32> m_free, m_pending;
		std::ofstream m_output, m_cpu_output;
		u32 m_cpu_events_this_frame = 0;
		u64 m_cpu_events_skipped = 0;
		std::mutex m_guard;
		std::unique_ptr<bounded_draw_attribution> m_draw_attribution;

		static u64 now()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void poll(bool device_idle = false)
		{
			// Bounded readback work. Completion proof also prevents reading the
			// previous availability bit while a newly reused reset is still queued.
			for (u32 count = 0; !m_pending.empty() && (device_idle || count < 16); ++count)
			{
				const u32 index = m_pending.front();
				auto& item = m_samples[index];
				if (!item.submitted)
					break;
				const bool reset_completed = item.commands->reset_id != item.generation;
				if (!device_idle && !reset_completed && (item.external_fence || !item.commands->poke()))
					break;

				// Each query returns its 64-bit result followed by 64-bit availability.
				u64 results[4]{};
				const auto status = vkGetQueryPoolResults(m_device, m_pool, index * 2, 2,
					sizeof(results), results, 2 * sizeof(u64),
					VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
				if (status == VK_NOT_READY || !results[1] || !results[3])
					break;
				if (status != VK_SUCCESS)
				{
					rsx_log.error("Vulkan timestamp diagnostics readback failed: %d", static_cast<s32>(status));
					break;
				}

				const u64 mask = m_valid_bits == 64 ? ~0ull : ((1ull << m_valid_bits) - 1);
				const u64 elapsed = (results[2] - results[0]) & mask;
				m_output << "sample," << item.frame << ',' << item.sequence << ',' << item.generation << ','
					<< item.record_begin << ',' << item.record_end << ',' << item.submit_begin << ',' << item.submit_end << ','
					<< now() << ',' << (results[0] & mask) << ',' << (results[2] & mask) << ','
					<< elapsed * m_period << ',' << m_period << ',' << m_valid_bits << ',' << m_skipped << ',' << item.reason << '\n';
				if (m_draw_attribution) m_draw_attribution->collect_after_original_completion(item.sequence);
				m_pending.pop_front();
				m_free.push_back(index);
			}
			if (m_draw_attribution) m_draw_attribution->finish_after_completed_window();
		}

	public:
		explicit timestamp_diagnostics(render_device& device)
		{
			m_device = device;
			m_period = device.gpu().get_limits().timestampPeriod;
			u32 family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device.gpu(), &family_count, nullptr);
			std::vector<VkQueueFamilyProperties> families(family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(device.gpu(), &family_count, families.data());
			if (device.get_graphics_queue_family() < family_count)
				m_valid_bits = families[device.get_graphics_queue_family()].timestampValidBits;
			if (!m_valid_bits || m_valid_bits > 64 || m_period <= 0)
			{
				rsx_log.warning("Vulkan timestamp diagnostics unavailable on this graphics queue");
				return;
			}
			if (const auto* flag = std::getenv("RPCS3_VK_B_DRAW_ATTRIBUTION"); flag && std::strcmp(flag, "1") == 0)
			{
				m_draw_attribution = std::make_unique<bounded_draw_attribution>(m_device, m_period, m_valid_bits);
				if (!m_draw_attribution->ready()) m_draw_attribution.reset();
			}
			VkQueryPoolCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			info.queryType = VK_QUERY_TYPE_TIMESTAMP;
			info.queryCount = slot_count * 2;
			if (vkCreateQueryPool(m_device, &info, nullptr, &m_pool) != VK_SUCCESS)
			{
				m_pool = VK_NULL_HANDLE;
				rsx_log.warning("Vulkan timestamp diagnostics query pool allocation failed");
				return;
			}
			const char* requested_path = std::getenv("RPCS3_VK_GPU_TIMESTAMP_PATH");
			const std::string path = requested_path && *requested_path ? requested_path : fs::get_cache_dir() + "vulkan-gpu-timestamps.csv";
			m_output.open(path, std::ios::out | std::ios::trunc);
			if (!m_output)
			{
				rsx_log.warning("Vulkan timestamp diagnostics could not open %s", path);
				vkDestroyQueryPool(m_device, m_pool, nullptr);
				m_pool = VK_NULL_HANDLE;
				return;
			}
			m_output << std::setprecision(17);
			m_output << "kind,frame_id,submission_id,command_reset_id,cpu_record_begin_ns,cpu_record_end_ns,cpu_submit_call_begin_ns,cpu_submit_call_end_ns,cpu_result_seen_ns,gpu_begin_tick,gpu_end_tick,gpu_command_span_ns,timestamp_period_ns,timestamp_valid_bits,skipped_total,submit_reason\n";
			m_cpu_output.open(path + ".cpu.csv", std::ios::out | std::ios::trunc);
			if (m_cpu_output)
				m_cpu_output << "kind,frame_id,submission_id,cpu_begin_ns,cpu_end_ns,label,skipped_total\n";
			for (u32 i = 0; i < slot_count; ++i)
				m_free.push_back(i);
			rsx_log.notice("Vulkan timestamp diagnostics enabled: %s (primary graphics CB spans; no added waits)", path);
		}

		~timestamp_diagnostics()
		{
			// Renderer calls finish_after_device_idle before destroying the CB chain.
			if (m_pool)
				vkDestroyQueryPool(m_device, m_pool, nullptr);
		}

		void begin(command_buffer_chunk& commands)
		{
			std::lock_guard lock(m_guard);
			if (!m_pool)
				return;
			poll();
			if (m_draw_attribution) m_draw_attribution->initialize(commands);
			if (m_active != invalid_slot || m_free.empty())
			{
				++m_skipped;
				return;
			}
			m_active = m_free.front();
			m_free.pop_front();
			m_samples[m_active] = {&commands, commands.reset_id, m_frame, ++m_sequence, now()};
			vkCmdResetQueryPool(commands, m_pool, m_active * 2, 2);
			vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool, m_active * 2);
		}

        bool draw_selected(command_buffer_chunk& commands)
        {
            if (!m_draw_attribution) return false;
            std::lock_guard lock(m_guard);
            return m_active != invalid_slot && m_samples[m_active].commands == &commands &&
                m_samples[m_active].frame == m_frame && m_draw_attribution->selected(commands, m_frame);
        }
        void draw_unsupported()
        {
            if (!m_draw_attribution) return;
            std::lock_guard lock(m_guard);
            m_draw_attribution->note_unsupported();
        }
        u32 draw_begin(command_buffer_chunk& commands, const bounded_draw_attribution::tag& tag,
            std::string_view vertex_source, std::string_view fragment_source)
        {
            if (!m_draw_attribution) return bounded_draw_attribution::invalid;
            std::lock_guard lock(m_guard);
            if (m_active == invalid_slot || m_samples[m_active].commands != &commands || m_samples[m_active].frame != m_frame) return bounded_draw_attribution::invalid;
            return m_draw_attribution->begin(commands, m_samples[m_active].sequence, m_frame, tag, vertex_source, fragment_source);
        }
        void draw_end(command_buffer_chunk& commands, u32 token)
        {
            if (!m_draw_attribution || token == bounded_draw_attribution::invalid) return;
            std::lock_guard lock(m_guard);
            const u64 sequence = m_active == invalid_slot ? 0 : m_samples[m_active].sequence;
            m_draw_attribution->end(commands, sequence, token);
        }
        void readback(command_buffer_chunk& commands, u32 address, u32 length, u32 width,
            u32 height, u32 pitch, u64 cookie, u64 image, bool armed)
        {
            if (!m_draw_attribution) return;
            std::lock_guard lock(m_guard);
            if (m_active == invalid_slot || m_samples[m_active].commands != &commands || m_samples[m_active].frame != m_frame) return;
            m_draw_attribution->readback(commands, m_samples[m_active].sequence, m_frame,
                address, length, width, height, pitch, cookie, image, armed);
        }

		void end(command_buffer_chunk& commands, const char* reason = "unspecified")
		{
			std::lock_guard lock(m_guard);
			if (m_active == invalid_slot || m_samples[m_active].commands != &commands)
				return;
			m_samples[m_active].record_end = now();
			m_samples[m_active].reason = reason;
			vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, m_active * 2 + 1);
		}

		void submit_begin(command_buffer_chunk& commands)
		{
			std::lock_guard lock(m_guard);
			if (m_active != invalid_slot && m_samples[m_active].commands == &commands)
				m_samples[m_active].submit_begin = now();
		}

		void submitted(command_buffer_chunk& commands, bool external_fence)
		{
			std::lock_guard lock(m_guard);
			if (m_active == invalid_slot || m_samples[m_active].commands != &commands)
				return;
			auto& item = m_samples[m_active];
			item.submit_end = now();
			item.external_fence = external_fence;
			item.submitted = true;
			m_pending.push_back(m_active);
			m_active = invalid_slot;
		}

		struct cpu_span
		{
			u64 frame = 0, submission = 0, begin = 0;
		};

		cpu_span begin_cpu_span()
		{
			std::lock_guard lock(m_guard);
			if (!m_pool || !m_cpu_output)
				return {};
			return {m_frame, m_active != invalid_slot ? m_samples[m_active].sequence : m_sequence, now()};
		}

		void end_cpu_span(cpu_span span, const char* label)
		{
			const u64 end = now();
			if (!span.begin)
				return;
			std::lock_guard lock(m_guard);
			// Bound output per present group; never hold the diagnostic mutex across a wait.
			if (m_cpu_events_this_frame >= 64)
			{
				++m_cpu_events_skipped;
				return;
			}
			++m_cpu_events_this_frame;
			m_cpu_output << "cpu_span," << span.frame << ',' << span.submission << ','
				<< span.begin << ',' << end << ',' << label << ',' << m_cpu_events_skipped << '\n';
		}

		void frame_end()
		{
			std::lock_guard lock(m_guard);
			if (!m_pool)
				return;
			m_output << "frame," << m_frame++ << ",0,0,0,0,0,0," << now() << ",0,0,0," << m_period << ',' << m_valid_bits << ',' << m_skipped << ",frame\n";
			m_cpu_events_this_frame = 0;
			if (!(m_frame % 60))
			{
				m_output.flush();
				m_cpu_output.flush();
			}
		}

		void finish_after_device_idle()
		{
			std::lock_guard lock(m_guard);
			if (m_pool)
			{
				poll(true);
				if (m_draw_attribution) m_draw_attribution->finish_after_original_device_idle();
				m_output.flush();
				m_cpu_output.flush();
			}
		}
	};
}
