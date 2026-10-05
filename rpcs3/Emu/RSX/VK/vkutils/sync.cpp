#include "barriers.h"
#include "buffer_object.h"
#include "commands.h"
#include "device.h"
#include "garbage_collector.h"
#include "sync.h"
#include "shared.h"

#include "Emu/Cell/timers.hpp"

#include "util/sysinfo.hpp"
#include "util/asm.hpp"
#include "util/logs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace vk
{
	namespace globals
	{
		static std::unique_ptr<gpu_debug_marker_pool> g_gpu_debug_marker_pool;
		static std::unique_ptr<gpu_label_pool> g_gpu_label_pool;

		gpu_debug_marker_pool& get_shared_marker_pool(const vk::render_device& dev)
		{
			if (!g_gpu_debug_marker_pool)
			{
				g_gpu_debug_marker_pool = std::make_unique<gpu_debug_marker_pool>(dev, 65536);
				vk::get_gc()->add_exit_callback([]()
				{
					g_gpu_debug_marker_pool.reset();
				});
			}

			return *g_gpu_debug_marker_pool;
		}

		gpu_label_pool& get_shared_label_pool(const vk::render_device& dev)
		{
			if (!g_gpu_label_pool)
			{
				g_gpu_label_pool = std::make_unique<gpu_label_pool>(dev, 65536);
				vk::get_gc()->add_exit_callback([]()
				{
					g_gpu_label_pool.reset();
				});
			}

			return *g_gpu_label_pool;
		}
	}

	// Util
	namespace v1_utils
	{
		VkPipelineStageFlags gather_src_stages(const VkDependencyInfoKHR& dependency)
		{
			VkPipelineStageFlags stages = VK_PIPELINE_STAGE_NONE;
			for (u32 i = 0; i < dependency.bufferMemoryBarrierCount; ++i)
			{
				stages |= dependency.pBufferMemoryBarriers[i].srcStageMask;
			}
			for (u32 i = 0; i < dependency.imageMemoryBarrierCount; ++i)
			{
				stages |= dependency.pImageMemoryBarriers[i].srcStageMask;
			}
			for (u32 i = 0; i < dependency.memoryBarrierCount; ++i)
			{
				stages |= dependency.pMemoryBarriers[i].srcStageMask;
			}
			return stages;
		}

		VkPipelineStageFlags gather_dst_stages(const VkDependencyInfoKHR& dependency)
		{
			VkPipelineStageFlags stages = VK_PIPELINE_STAGE_NONE;
			for (u32 i = 0; i < dependency.bufferMemoryBarrierCount; ++i)
			{
				stages |= dependency.pBufferMemoryBarriers[i].dstStageMask;
			}
			for (u32 i = 0; i < dependency.imageMemoryBarrierCount; ++i)
			{
				stages |= dependency.pImageMemoryBarriers[i].dstStageMask;
			}
			for (u32 i = 0; i < dependency.memoryBarrierCount; ++i)
			{
				stages |= dependency.pMemoryBarriers[i].dstStageMask;
			}
			return stages;
		}

		auto get_memory_barriers(const VkDependencyInfoKHR& dependency)
		{
			std::vector<VkMemoryBarrier> result;
			for (u32 i = 0; i < dependency.memoryBarrierCount; ++i)
			{
				result.push_back
				({
					VK_STRUCTURE_TYPE_MEMORY_BARRIER,
					nullptr,
					static_cast<VkAccessFlags>(dependency.pMemoryBarriers[i].srcAccessMask),
					static_cast<VkAccessFlags>(dependency.pMemoryBarriers[i].dstAccessMask)
				});
			}
			return result;
		}

		auto get_image_memory_barriers(const VkDependencyInfoKHR& dependency)
		{
			std::vector<VkImageMemoryBarrier> result;
			for (u32 i = 0; i < dependency.imageMemoryBarrierCount; ++i)
			{
				result.push_back
				({
					VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					nullptr,
					static_cast<VkAccessFlags>(dependency.pImageMemoryBarriers[i].srcAccessMask),
					static_cast<VkAccessFlags>(dependency.pImageMemoryBarriers[i].dstAccessMask),
					dependency.pImageMemoryBarriers[i].oldLayout,
					dependency.pImageMemoryBarriers[i].newLayout,
					dependency.pImageMemoryBarriers[i].srcQueueFamilyIndex,
					dependency.pImageMemoryBarriers[i].dstQueueFamilyIndex,
					dependency.pImageMemoryBarriers[i].image,
					dependency.pImageMemoryBarriers[i].subresourceRange
				});
			}
			return result;
		}

		auto get_buffer_memory_barriers(const VkDependencyInfoKHR& dependency)
		{
			std::vector<VkBufferMemoryBarrier> result;
			for (u32 i = 0; i < dependency.bufferMemoryBarrierCount; ++i)
			{
				result.push_back
				({
					VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
					nullptr,
					static_cast<VkAccessFlags>(dependency.pBufferMemoryBarriers[i].srcAccessMask),
					static_cast<VkAccessFlags>(dependency.pBufferMemoryBarriers[i].dstAccessMask),
					dependency.pBufferMemoryBarriers[i].srcQueueFamilyIndex,
					dependency.pBufferMemoryBarriers[i].dstQueueFamilyIndex,
					dependency.pBufferMemoryBarriers[i].buffer,
					dependency.pBufferMemoryBarriers[i].offset,
					dependency.pBufferMemoryBarriers[i].size
				});
			}
			return result;
		}
	}

	// Objects
	fence::fence(VkDevice dev)
	{
		owner                  = dev;
		VkFenceCreateInfo info = {};
		info.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		CHECK_RESULT(vkCreateFence(dev, &info, nullptr, &handle));
	}

	fence::~fence()
	{
		if (handle)
		{
			vkDestroyFence(owner, handle, nullptr);
			handle = VK_NULL_HANDLE;
		}
	}

	void fence::reset()
	{
		vkResetFences(owner, 1, &handle);
		flushed.release(false);
	}

	void fence::signal_flushed()
	{
		flushed.release(true);
	}

	void fence::wait_flush()
	{
		while (!flushed)
		{
			utils::pause();
		}
	}

	fence::operator bool() const
	{
		return (handle != VK_NULL_HANDLE);
	}

	semaphore::semaphore(const render_device& dev)
		: m_device(dev)
	{
		VkSemaphoreCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		CHECK_RESULT(vkCreateSemaphore(m_device, &info, nullptr, &m_handle));
	}

	semaphore::~semaphore()
	{
		vkDestroySemaphore(m_device, m_handle, nullptr);
	}

	semaphore::operator VkSemaphore() const
	{
		return m_handle;
	}

	event::event(const render_device& dev, sync_domain domain)
		: m_device(&dev), m_domain(domain)
	{
		m_backend = dev.get_synchronization2_support()
			? sync_backend::events_v2
			: sync_backend::events_v1;

		if (domain == sync_domain::host &&
			vk::get_driver_vendor() == vk::driver_vendor::AMD &&
			vk::get_chip_family() < vk::chip_class::AMD_navi1x)
		{
			// Events don't work quite right on AMD drivers
			m_backend = sync_backend::gpu_label;

			m_label = std::make_unique<vk::gpu_label>(globals::get_shared_label_pool(dev));
			return;
		}

		VkEventCreateInfo info
		{
			.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0
		};

		if (domain == sync_domain::gpu && m_backend == sync_backend::events_v2)
		{
			info.flags = VK_EVENT_CREATE_DEVICE_ONLY_BIT_KHR;
		}

		CHECK_RESULT(vkCreateEvent(dev, &info, nullptr, &m_vk_event));
	}

	event::~event()
	{
		if (m_vk_event) [[likely]]
		{
			vkDestroyEvent(*m_device, m_vk_event, nullptr);
		}
	}

	void event::resolve_dependencies(const command_buffer& cmd, const VkDependencyInfoKHR& dependency)
	{
		ensure(m_backend != sync_backend::gpu_label);

		if (m_backend == sync_backend::events_v2)
		{
			_vkCmdPipelineBarrier2KHR(cmd, &dependency);
			return;
		}

		const auto src_stages = v1_utils::gather_src_stages(dependency);
		const auto dst_stages = v1_utils::gather_dst_stages(dependency);
		const auto memory_barriers = v1_utils::get_memory_barriers(dependency);
		const auto image_memory_barriers = v1_utils::get_image_memory_barriers(dependency);
		const auto buffer_memory_barriers = v1_utils::get_buffer_memory_barriers(dependency);

		vkCmdPipelineBarrier(cmd, src_stages, dst_stages, dependency.dependencyFlags,
			::size32(memory_barriers), memory_barriers.data(),
			::size32(buffer_memory_barriers), buffer_memory_barriers.data(),
			::size32(image_memory_barriers), image_memory_barriers.data());
	}

	void event::signal(const command_buffer& cmd, const VkDependencyInfoKHR& dependency)
	{
		if (m_backend == sync_backend::gpu_label)
		{
			// Fallback path
			m_label->signal(cmd, dependency);
			return;
		}

		if (m_domain != sync_domain::host)
		{
			// As long as host is not involved, keep things consistent.
			// The expectation is that this will be awaited using the gpu_wait function.
			if (m_backend == sync_backend::events_v2) [[ likely ]]
			{
				_vkCmdSetEvent2KHR(cmd, m_vk_event, &dependency);
			}
			else
			{
				const auto dst_stages = v1_utils::gather_dst_stages(dependency);
				vkCmdSetEvent(cmd, m_vk_event, dst_stages);
			}

			return;
		}

		// Host sync doesn't behave intuitively with events, so we use some workarounds.
		// 1. Resolve the actual dependencies on a pipeline barrier.
		resolve_dependencies(cmd, dependency);

		// 2. Signalling won't wait. The caller is responsible for setting up the dependencies correctly.
		if (m_backend != sync_backend::events_v2)
		{
			vkCmdSetEvent(cmd, m_vk_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			return;
		}

		// We need a memory barrier to keep AMDVLK from hanging
		VkMemoryBarrier2KHR mem_barrier =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR,
			.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
		};

		// Empty dependency that does nothing
		VkDependencyInfoKHR empty_dependency
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &mem_barrier
		};

		_vkCmdSetEvent2KHR(cmd, m_vk_event, &empty_dependency);
	}

	void event::host_signal() const
	{
		if (m_backend != sync_backend::gpu_label) [[ likely ]]
		{
			vkSetEvent(*m_device, m_vk_event);
			return;
		}

		m_label->set();
	}

	void event::gpu_wait(const command_buffer& cmd, const VkDependencyInfoKHR& dependency) const
	{
		ensure(m_domain != sync_domain::host);

		if (m_backend == sync_backend::events_v2) [[ likely ]]
		{
			_vkCmdWaitEvents2KHR(cmd, 1, &m_vk_event, &dependency);
			return;
		}

		const auto src_stages = v1_utils::gather_src_stages(dependency);
		const auto dst_stages = v1_utils::gather_dst_stages(dependency);
		const auto memory_barriers = v1_utils::get_memory_barriers(dependency);
		const auto image_memory_barriers = v1_utils::get_image_memory_barriers(dependency);
		const auto buffer_memory_barriers = v1_utils::get_buffer_memory_barriers(dependency);

		vkCmdWaitEvents(cmd,
			1, &m_vk_event,
			src_stages, dst_stages,
			::size32(memory_barriers), memory_barriers.data(),
			::size32(buffer_memory_barriers), buffer_memory_barriers.data(),
			::size32(image_memory_barriers), image_memory_barriers.data());
	}

	void event::reset() const
	{
		if (m_backend != sync_backend::gpu_label) [[ likely ]]
		{
			vkResetEvent(*m_device, m_vk_event);
			return;
		}

		m_label->reset();
	}

	VkResult event::status() const
	{
		if (m_backend != sync_backend::gpu_label) [[ likely ]]
		{
			return vkGetEventStatus(*m_device, m_vk_event);
		}

		return m_label->signaled() ? VK_EVENT_SET : VK_EVENT_RESET;
	}

	gpu_label_pool::gpu_label_pool(const vk::render_device& dev, u32 count)
		: pdev(&dev), m_count(count)
	{}

	gpu_label_pool::~gpu_label_pool()
	{
		if (m_mapped)
		{
			ensure(m_buffer);
			m_buffer->unmap();
		}
	}

	std::tuple<VkBuffer, u64, volatile u32*> gpu_label_pool::allocate()
	{
		if (!m_buffer || m_offset >= m_count)
		{
			create_impl();
		}

		const auto out_offset = m_offset;
		m_offset ++;
		return { m_buffer->value, out_offset * 4, m_mapped + out_offset };
	}

	void gpu_label_pool::create_impl()
	{
		if (m_buffer)
		{
			m_buffer->unmap();
			vk::get_gc()->dispose(m_buffer);
		}

		m_buffer = std::make_unique<buffer>
		(
			*pdev,
			m_count * 4,
			pdev->get_memory_mapping().host_visible_coherent,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			0,
			VMM_ALLOCATION_POOL_SYSTEM
		);

		m_mapped = reinterpret_cast<volatile u32*>(m_buffer->map(0, VK_WHOLE_SIZE));
		m_offset = 0;
	}

	gpu_label::gpu_label(gpu_label_pool& pool)
	{
		std::tie(m_buffer_handle, m_buffer_offset, m_ptr) = pool.allocate();
		reset();
	}

	gpu_label::~gpu_label()
	{
		m_ptr = nullptr;
		m_buffer_offset = 0;
		m_buffer_handle = VK_NULL_HANDLE;
	}

	void gpu_label::signal(const vk::command_buffer& cmd, const VkDependencyInfoKHR& dependency)
	{
		const auto src_stages = v1_utils::gather_src_stages(dependency);
		auto dst_stages = v1_utils::gather_dst_stages(dependency);
		auto memory_barriers = v1_utils::get_memory_barriers(dependency);
		const auto image_memory_barriers = v1_utils::get_image_memory_barriers(dependency);
		const auto buffer_memory_barriers = v1_utils::get_buffer_memory_barriers(dependency);

		// Ensure wait before filling the label
		dst_stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		if (memory_barriers.empty())
		{
			VkMemoryBarrier signal_barrier =
			{
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
			};
			memory_barriers.push_back(std::move(signal_barrier));
		}
		else
		{
			auto& barrier = memory_barriers.front();
			barrier.dstAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
		}

		vkCmdPipelineBarrier(cmd, src_stages, dst_stages, dependency.dependencyFlags,
			::size32(memory_barriers), memory_barriers.data(),
			::size32(buffer_memory_barriers), buffer_memory_barriers.data(),
			::size32(image_memory_barriers), image_memory_barriers.data());

		vkCmdFillBuffer(cmd, m_buffer_handle, m_buffer_offset, 4, label_constants::set_);
	}

	gpu_debug_marker::gpu_debug_marker(gpu_debug_marker_pool& pool, std::string message)
		: gpu_label(pool), m_message(std::move(message))
	{}

	gpu_debug_marker::~gpu_debug_marker()
	{
		if (!m_printed)
		{
			dump();
		}
	}

	void gpu_debug_marker::dump()
	{
		if (*m_ptr == gpu_label::label_constants::reset_)
		{
			rsx_log.error("DEBUG MARKER NOT REACHED: %s", m_message);
		}

		m_printed = true;
	}

	void gpu_debug_marker::dump() const
	{
		if (*m_ptr == gpu_label::label_constants::reset_)
		{
			rsx_log.error("DEBUG MARKER NOT REACHED: %s", m_message);
		}
		else
		{
			rsx_log.error("DEBUG MARKER: %s", m_message);
		}
	}

	void gpu_debug_marker::insert(
		const vk::render_device& dev,
		const vk::command_buffer& cmd,
		std::string message,
		VkPipelineStageFlags stages,
		VkAccessFlags access)
	{
		VkMemoryBarrier2KHR barrier =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR,
			.srcStageMask = stages,
			.srcAccessMask = access,
			.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
		};

		VkDependencyInfoKHR dependency =
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &barrier
		};

		auto result = std::make_unique<gpu_debug_marker>(globals::get_shared_marker_pool(dev), message);
		result->signal(cmd, dependency);
		vk::get_gc()->dispose(result);
	}

	debug_marker_scope::debug_marker_scope(const vk::command_buffer& cmd, const std::string& message)
		: m_device(&cmd.get_command_pool().get_owner()), m_cb(&cmd), m_message(message), m_tag(rsx::get_shared_tag())
	{
		vk::gpu_debug_marker::insert(
			*m_device,
			*m_cb,
			fmt::format("0x%llx: Enter %s", m_tag, m_message)
		);
	}

	debug_marker_scope::~debug_marker_scope()
	{
		ensure(m_cb && m_cb->is_recording());

		vk::gpu_debug_marker::insert(
			*m_device,
			*m_cb,
			fmt::format("0x%x: Exit %s", m_tag, m_message)
		);
	}

	VkResult wait_for_fence(fence* pFence, u64 timeout)
	{
		pFence->wait_flush();

		if (timeout)
		{
			return vkWaitForFences(*g_render_device, 1, &pFence->handle, VK_FALSE, timeout * 1000ull);
		}
		else
		{
			while (auto status = vkGetFenceStatus(*g_render_device, pFence->handle))
			{
				switch (status)
				{
				case VK_NOT_READY:
					utils::pause();
					continue;
				default:
					die_with_error(status);
					return status;
				}
			}

			return VK_SUCCESS;
		}
	}

	namespace
	{
		enum class event_wait_backoff { none, yield, sleep, mwait };
		struct event_wait_options
		{
			event_wait_backoff mode = event_wait_backoff::none;
			u64 spin_us = 50;
			bool statistics = false;
		};

		const event_wait_options& get_event_wait_options()
		{
			static const auto options = []
			{
				event_wait_options result;
				if (const char* mode = std::getenv("RPCS3_EXPERIMENT_EVENT_WAIT"))
				{
					if (std::strcmp(mode, "yield") == 0) result.mode = event_wait_backoff::yield;
					if (std::strcmp(mode, "sleep") == 0) result.mode = event_wait_backoff::sleep;
					if (std::strcmp(mode, "mwait") == 0) result.mode = event_wait_backoff::mwait;
				}
				if (const char* value = std::getenv("RPCS3_EXPERIMENT_EVENT_SPIN_US"))
				{
					char* end = nullptr;
					const u64 parsed = std::strtoull(value, &end, 10);
					if (end != value && !*end && parsed <= 10000) result.spin_us = parsed;
				}
				if (const char* value = std::getenv("RPCS3_EXPERIMENT_EVENT_WAIT_STATS"))
				{
					result.statistics = std::strcmp(value, "1") == 0;
				}
				return result;
			}();
			return options;
		}

		void record_event_wait(u64 elapsed_us, u64 polls, u64 backoffs, VkResult result)
		{
			// Thread-local aggregation avoids cacheline contention among SPU workers.
			struct totals
			{
				u64 count = 0, time_us = 0, max_us = 0, polls = 0, backoffs = 0, failures = 0;
				std::array<u64, 8> buckets{};
			};
			thread_local totals stats;
			constexpr std::array<u64, 7> limits = { 1, 5, 10, 50, 100, 500, 1000 };
			usz bucket = 0;
			while (bucket < limits.size() && elapsed_us > limits[bucket]) ++bucket;
			++stats.buckets[bucket];
			++stats.count;
			stats.time_us += elapsed_us;
			stats.max_us = std::max(stats.max_us, elapsed_us);
			stats.polls += polls;
			stats.backoffs += backoffs;
			stats.failures += result != VK_SUCCESS;
			if (stats.count == 128 || (stats.count % 1024) == 0)
			{
				rsx_log.notice("Event wait stats: n=%llu time_us=%llu max_us=%llu polls=%llu backoffs=%llu failures=%llu buckets_us[<=1,<=5,<=10,<=50,<=100,<=500,<=1000,>1000]=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]",
					stats.count, stats.time_us, stats.max_us, stats.polls, stats.backoffs, stats.failures,
					stats.buckets[0], stats.buckets[1], stats.buckets[2], stats.buckets[3],
					stats.buckets[4], stats.buckets[5], stats.buckets[6], stats.buckets[7]);
			}
		}

		VkResult wait_for_event_experimental(event* pEvent, u64 timeout_us, const event_wait_options& options)
		{
			const u64 freq = utils::get_tsc_freq();
			// Use the same time units and timeout conversion as the stock loop.
			const u64 ticks_per_us = freq ? freq / 1'000'000 : 1;
			const u64 timeout = timeout_us * ticks_per_us;
			const auto clock = [freq]() { return freq ? utils::get_tsc() : get_system_time(); };
			const u64 entered = clock();
			u64 start = 0, polls = 0, backoffs = 0;
			const auto finish = [&](VkResult result)
			{
				if (options.statistics)
				{
					const u64 now = clock();
					record_event_wait(now >= entered ? (now - entered) / ticks_per_us : 0, polls, backoffs, result);
				}
				return result;
			};
			while (true)
			{
				++polls;
				switch (const auto status = pEvent->status())
				{
				case VK_EVENT_SET: return finish(VK_SUCCESS);
				case VK_EVENT_RESET: break;
				default:
					die_with_error(status);
					return finish(status);
				}
				const u64 now = clock();
				if (timeout)
				{
					if (!start)
					{
						start = now;
						continue;
					}
					if ((now > start) && (now - start) > timeout)
					{
						rsx_log.error("[vulkan] vk::wait_for_event has timed out!");
						return finish(VK_TIMEOUT);
					}
				}
				if (options.mode != event_wait_backoff::none && now >= entered &&
					(now - entered) >= options.spin_us * ticks_per_us)
				{
					if (options.mode == event_wait_backoff::yield)
					{
						++backoffs;
						std::this_thread::yield();
					}
					else if (!timeout || (now - start) + ticks_per_us <= timeout)
					{
						// Request only 1us; host sleep may wake later due to scheduling.
						++backoffs;
						if (options.mode == event_wait_backoff::mwait)
						{
							// Monitor only our own isolated CPU cacheline. The Vulkan
							// event stays opaque and is checked again after the timer.
							thread_local atomic_t<u32, 64> idle_word{0};
							utils::spin_on_cacheline_once(idle_word, u32{0}, 1);
						}
						else
						{
							std::this_thread::sleep_for(std::chrono::microseconds(1));
						}
					}
					else
					{
						utils::pause();
					}
				}
				else
				{
					utils::pause();
				}
			}
		}
	}

	VkResult wait_for_event(event* pEvent, u64 timeout)
	{
		const auto& options = get_event_wait_options();
		if (options.mode != event_wait_backoff::none || options.statistics)
		{
			return wait_for_event_experimental(pEvent, timeout, options);
		}

		// Convert timeout to TSC cycles. Timeout accuracy isn't super-important, only fast response when event is signaled (within 10us if possible)
		const u64 freq = utils::get_tsc_freq();

		if (freq)
		{
			timeout *= (freq / 1'000'000);
		}

		u64 start = 0;

		while (true)
		{
			switch (const auto status = pEvent->status())
			{
			case VK_EVENT_SET:
				return VK_SUCCESS;
			case VK_EVENT_RESET:
				break;
			default:
				die_with_error(status);
				return status;
			}

			if (timeout)
			{
				const auto now = freq ? utils::get_tsc() : get_system_time();

				if (!start)
				{
					start = now;
					continue;
				}

				if ((now > start) &&
					(now - start) > timeout)
				{
					rsx_log.error("[vulkan] vk::wait_for_event has timed out!");
					return VK_TIMEOUT;
				}
			}

			utils::pause();
		}
	}
}
