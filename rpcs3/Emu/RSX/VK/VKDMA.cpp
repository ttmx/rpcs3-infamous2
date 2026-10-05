#include "stdafx.h"
#include "VKResourceManager.h"
#include "VKDMA.h"
#include "VKLiveCtl.hpp"
#include "VKReadbackCopy.hpp"
#include "VKReadbackOOPControl.hpp"
#include "../Common/readback_chain_diagnostics.hpp"
#include "VKHelpers.h"
#include "VKUploadDiagnostics.hpp"
#include "VKRTTLoadDiagnostics.hpp"
#include "VKRTTBackgroundProbe.hpp"
#include "vkutils/device.h"

#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXThread.h"
#include "Utilities/mutex.h"

#include "util/asm.hpp"
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <atomic>

namespace vk
{
	static constexpr usz s_dma_block_length = 0x00010000;
	static constexpr u32 s_dma_block_mask   = 0xFFFF0000;

	std::unordered_map<u32, std::unique_ptr<dma_block>> g_dma_pool;
	shared_mutex g_dma_mutex;

	// Validation
	atomic_t<u64> s_allocated_dma_pool_size{ 0 };

	static bool compare_dma_enabled()
	{
		static const bool enabled = []
		{
			const char* value = std::getenv("RPCS3_EXPERIMENT_COMPARE_DMA");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}

	dma_block::~dma_block()
	{
		// Use safe free (uses gc to clean up)
		free();
	}

	void* dma_block::map_range(const utils::address_range32& range)
	{
		if (inheritance_info.parent)
		{
			return inheritance_info.parent->map_range(range);
		}

		if (memory_mapping == nullptr)
		{
			memory_mapping = static_cast<u8*>(allocated_memory->map(0, VK_WHOLE_SIZE));
			ensure(memory_mapping);
		}

		ensure(range.start >= base_address);
		u32 start = range.start;
		start -= base_address;
		return memory_mapping + start;
	}

	void dma_block::unmap()
	{
		if (inheritance_info.parent)
		{
			inheritance_info.parent->unmap();
		}
		else
		{
			allocated_memory->unmap();
			memory_mapping = nullptr;
		}
	}

	void dma_block::allocate(const render_device& dev, usz size)
	{
		// Acquired blocks are always to be assumed dirty. It is not possible to synchronize host access and inline
		// buffer copies without causing weird issues. Overlapped incomplete data ends up overwriting host-uploaded data.
		free();

		allocated_memory = std::make_unique<vk::buffer>(dev, size,
			dev.get_memory_mapping().host_visible_coherent, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				(readback_oop_enabled() ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0), 0,
			VMM_ALLOCATION_POOL_UNDEFINED);

		// Initialize memory contents. This isn't something that happens often.
		// Pre-loading the contents helps to avoid leakage when mixed types of allocations are in use (NVIDIA)
		// TODO: Fix memory lost when old object goes out of use with in-flight data.
		auto dst = static_cast<u8*>(allocated_memory->map(0, size));
		auto src = vm::get_super_ptr<u8>(base_address);

		if (rsx::get_location(base_address) == CELL_GCM_LOCATION_LOCAL ||
			vm::check_addr(base_address, 0, static_cast<u32>(size))) [[ likely ]]
		{
			// Linear virtual memory space. Copy all at once.
			std::memcpy(dst, src, size);
		}
		else
		{
			// Some games will have address holes in the range and page in data using faults.
			// Copy page by page. Slow, but we only really have to do this a handful of times.
			// Note that base_address is 16k aligned.
			for (u32 address = base_address; address < (base_address + size); address += 4096, src += 4096, dst += 4096)
			{
				if (vm::check_addr(address, 0))
				{
					std::memcpy(dst, src, 4096);
				}
			}
		}

		allocated_memory->unmap();

		s_allocated_dma_pool_size += allocated_memory->size();
	}

	void dma_block::free()
	{
		if (allocated_memory)
		{
			// Do some accounting before the allocation info is no more
			s_allocated_dma_pool_size -= allocated_memory->size();

			// If you have both a memory allocation AND a parent block at the same time, you're in trouble
			ensure(head() == this);

			if (memory_mapping)
			{
				// vma allocator does not allow us to destroy mapped memory on windows
				unmap();
				ensure(!memory_mapping);
			}

			// Move allocation to gc
			auto gc = vk::get_resource_manager();
			gc->dispose(allocated_memory);
		}
	}

	void dma_block::init(const render_device& dev, u32 addr, usz size)
	{
		ensure((size > 0) && !((size | addr) & ~s_dma_block_mask));
		base_address = addr;

		allocate(dev, size);
		ensure(!inheritance_info.parent);
	}

	void dma_block::init(dma_block* parent, u32 addr, usz size)
	{
		ensure((size > 0) && !((size | addr) & ~s_dma_block_mask));

		base_address = addr;
		inheritance_info.parent = parent;
		inheritance_info.block_offset = (addr - parent->base_address);
	}

	void dma_block::flush(const utils::address_range32& range, const dma_source_observer* observer)
	{
		if (inheritance_info.parent)
		{
			// Parent may be a different type of block
			inheritance_info.parent->flush(range, observer);
			return;
		}

		rsx::readback_chain_trace::scope chain_copy("readback_dma_cpu_copy");
		chain_copy.range(range.start,range.length());
		if(chain_copy)
		{
		 const auto type=allocated_memory->memory->diagnostic_memory_type();
		 const auto flags=type<VK_MAX_MEMORY_TYPES?g_render_device->gpu().get_memory_properties().memoryTypes[type].propertyFlags:0;
		 chain_copy.image(allocated_memory.get());chain_copy.auxiliary(type,flags);
		}
		auto src = map_range(range);
		if (observer && observer->observe)
			observer->observe(observer->context, src, allocated_memory.get(), range.start - base_address, range.start, range.length());
		auto dst = vm::get_super_ptr(range.start);
		bool copied = false;
		if (readback_copy::enabled())
		{
			static std::atomic<bool> disabled{false};
			const auto type = allocated_memory->memory->diagnostic_memory_type();
			const auto flags = type < VK_MAX_MEMORY_TYPES ?
				g_render_device->gpu().get_memory_properties().memoryTypes[type].propertyFlags : 0;
			if (!disabled.load(std::memory_order_relaxed))
				copied = readback_copy::copy(dst, src, range.length(), flags, utils::has_avx512());
			if (copied)
			{
				static std::atomic<bool> announced{false};
				if (!announced.load(std::memory_order_relaxed) && !announced.exchange(true, std::memory_order_relaxed))
					rsx_log.notice("Readback streaming copy activated: address=0x%x bytes=%u memory_type=%u flags=0x%x", range.start, range.length(), type, flags);
				if (readback_copy::validation_enabled())
				{
					// Diagnostic only: sample real completed staging bytes under the
					// original cache/DMA locks before the original VM unprotect.
					static std::atomic<u32> checks{0};
					const u32 check = checks.fetch_add(1, std::memory_order_relaxed);
					if (check < 32)
					{
						if (std::memcmp(dst, src, range.length()) != 0)
						{
							disabled.store(true, std::memory_order_relaxed);
							copied = false;
							rsx_log.error("Readback streaming validation mismatch: check=%u address=0x%x bytes=%u; reverting to stock memcpy", check + 1, range.start, range.length());
						}
						else
							rsx_log.notice("Readback streaming validation matched real bytes: check=%u address=0x%x bytes=%u", check + 1, range.start, range.length());
					}
				}
			}
		}
		if (!copied)
			std::memcpy(dst, src, range.length());

		// NOTE: Do not unmap. This can be extremely slow on some platforms.
	}

	void dma_block::load(const utils::address_range32& range)
	{
		if (inheritance_info.parent)
		{
			// Parent may be a different type of block
			inheritance_info.parent->load(range);
			return;
		}

		auto src = vm::get_super_ptr(range.start);
		auto dst = map_range(range);
		if (upload_diagnostics::active())
		{
			const auto type = allocated_memory->memory->diagnostic_memory_type();
			const auto flags = type < VK_MAX_MEMORY_TYPES ? g_render_device->gpu().get_memory_properties().memoryTypes[type].propertyFlags : 0;
			std::ostringstream out;
			out << "dma_load," << upload_diagnostics::now_ns() << ',' << vk::get_current_frame_id() << ",0,0," << range.start
				<< ',' << range.length() << ",0,0,0,," << allocated_memory->uid() << ',' << type << ',' << flags
				<< ",0,0,0," << range.length() << ",0,0,0,0,0,0,0\n";
			upload_diagnostics::writer().write(out.str(), 1);
		}

		if (compare_dma_enabled() && !m_gpu_written && range.length() >= 65536)
		{
			// Avoid reading an uncached host upload allocation. All eligible allocator
			// types must be CPU-cached; the stock upload heap can fall back to uncached.
			const auto& dev = *g_render_device;
			const auto& properties = dev.gpu().get_memory_properties();
			bool cached = true;
			for (const auto type : dev.get_memory_mapping().host_visible_coherent)
			{
				constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				cached &= (properties.memoryTypes[type].propertyFlags & required) == required;
			}
			if (cached)
			{
				// No hashes or dirty-tag assumptions: elide writes only after exact equality.
				const bool unchanged = std::memcmp(dst, src, range.length()) == 0;
				struct compare_stats
				{
					u64 calls = 0;
					u64 unchanged_calls = 0;
					u64 checked_bytes = 0;
					u64 elided_bytes = 0;
				};
				thread_local compare_stats stats;
				++stats.calls;
				stats.checked_bytes += range.length();
				if (unchanged)
				{
					++stats.unchanged_calls;
					stats.elided_bytes += range.length();
				}
				if (stats.calls == 1 || !(stats.calls % 128))
				{
					rsx_log.notice("DMA comparison: %llu/%llu unchanged loads, %llu/%llu bytes elided",
						stats.unchanged_calls, stats.calls, stats.elided_bytes, stats.checked_bytes);
				}
				if (unchanged)
				{
					return;
				}
			}
		}
		if (const u64 stream_min = live_ctl::get(1); stream_min && range.length() >= stream_min && utils::has_avx512())
		{
			// Large loads overwrite the whole destination; skip the read-for-ownership and keep the cache clean.
			if (live_ctl::get(3) & 4)
			{
				constexpr u32 chunk = 0x8000;
				const auto* from = static_cast<const u8*>(static_cast<const void*>(src));
				auto* to = static_cast<u8*>(dst);
				live_ctl::prefetch_range(from, range.length(), chunk);
				for (u32 offset = 0; offset < range.length(); offset += chunk)
				{
					const u32 remaining = range.length() - offset;
					if (remaining > chunk)
					{
						live_ctl::prefetch_range(from + offset + chunk, remaining - chunk, chunk);
					}
					readback_copy::stream(to + offset, from + offset, std::min(remaining, chunk));
				}
			}
			else
			{
				readback_copy::stream(dst, src, range.length());
			}
		}
		else
		{
			std::memcpy(dst, src, range.length());
		}
		if (rtt_background_probe::enabled())
		{
			// Metadata comes from the existing armed RTT scope. No extra map/VM read.
			const auto* context = rtt_load_diagnostics::current_context;
			if (context)
			{
				const auto& v = context->values;
				using namespace rtt_load_diagnostics;
				if (v[blit_active] && v[discard_allowed] && !v[integrity_reload] &&
					!v[tiled] && !v[swizzled] && v[spp] == 1 && v[scale] == 100 &&
					!v[old_contents] && v[address] == range.start && v[length] == range.length())
				{
					const auto type = allocated_memory->memory->diagnostic_memory_type();
					const auto flags = type < VK_MAX_MEMORY_TYPES ? g_render_device->gpu().get_memory_properties().memoryTypes[type].propertyFlags : 0;
					const rtt_background_probe::key identity{ v[image_uid], v[address], v[width], v[height], v[pitch], v[gcm_format], v[host_format], v[aspect], v[x1], v[y1], v[x2], v[y2] };
					// Existing DMA reader lock/map remain held. GPU writer quiescence is
					// not proven; samples establish observed byte stability only.
					rtt_background_probe::observe(identity, dst, context->frame, flags);
				}
			}
		}

		// NOTE: Do not unmap. This can be extremely slow on some platforms.
	}

	dma_mapping_handle dma_block::get(const utils::address_range32& range)
	{
		if (inheritance_info.parent)
		{
			return inheritance_info.parent->get(range);
		}

		ensure(range.start >= base_address);
		ensure(range.end <= end());

		// mark_dirty(range);
		return { (range.start - base_address), allocated_memory.get() };
	}

	dma_block* dma_block::head()
	{
		if (!inheritance_info.parent)
			return this;

		return inheritance_info.parent->head();
	}

	const dma_block* dma_block::head() const
	{
		if (!inheritance_info.parent)
			return this;

		return inheritance_info.parent->head();
	}

	void dma_block::set_parent(dma_block* parent)
	{
		ensure(parent);
		ensure(parent->base_address < base_address);
		if (inheritance_info.parent == parent)
		{
			// Nothing to do
			return;
		}

		if (allocated_memory)
		{
			// Acquired blocks are always to be assumed dirty. It is not possible to synchronize host access and inline
			// buffer copies without causing weird issues. Overlapped incomplete data ends up overwriting host-uploaded data.
			free();
		}

		inheritance_info.parent = parent;
		inheritance_info.block_offset = (base_address - parent->base_address);
	}

	void dma_block::extend(const render_device& dev, usz new_size)
	{
		ensure(allocated_memory);
		if (new_size <= allocated_memory->size())
			return;

		allocate(dev, new_size);
	}

	u32 dma_block::start() const
	{
		return base_address;
	}

	u32 dma_block::end() const
	{
		auto source = head();
		return (source->base_address + source->allocated_memory->size() - 1);
	}

	u32 dma_block::size() const
	{
		return (allocated_memory) ? allocated_memory->size() : 0;
	}

	void dma_block_EXT::allocate(const render_device& dev, usz size)
	{
		// Acquired blocks are always to be assumed dirty. It is not possible to synchronize host access and inline
		// buffer copies without causing weird issues. Overlapped incomplete data ends up overwriting host-uploaded data.
		free();

		allocated_memory = std::make_unique<vk::buffer>(dev,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			vm::get_super_ptr<void>(base_address),
			size);

		s_allocated_dma_pool_size += allocated_memory->size();
	}

	void* dma_block_EXT::map_range(const utils::address_range32& range)
	{
		return vm::get_super_ptr<void>(range.start);
	}

	void dma_block_EXT::unmap()
	{
		// NOP
	}

	void dma_block_EXT::flush(const utils::address_range32&, const dma_source_observer*)
	{
		// NOP
	}

	void dma_block_EXT::load(const utils::address_range32&)
	{
		// NOP
	}

	bool test_host_pointer([[maybe_unused]] u32 base_address, [[maybe_unused]] usz length)
	{
#ifdef _WIN32
		MEMORY_BASIC_INFORMATION mem_info;
		if (!::VirtualQuery(vm::get_super_ptr<const void>(base_address), &mem_info, sizeof(mem_info)))
		{
			rsx_log.error("VirtualQuery failed! LastError=%s", fmt::win_error{GetLastError(), nullptr});
			return false;
		}

		return (mem_info.RegionSize >= length);
#else
		return true; // *nix behavior is unknown with NVIDIA drivers
#endif
	}

	void create_dma_block(std::unique_ptr<dma_block>& block, u32 base_address, usz expected_length)
	{
		bool allow_host_buffers = false;
		if (rsx::get_current_renderer()->get_backend_config().supports_passthrough_dma)
		{
			allow_host_buffers =
#if defined(_WIN32)
				(vk::get_driver_vendor() == driver_vendor::NVIDIA) ?
					test_host_pointer(base_address, expected_length) :
#endif
				true;

			if (!allow_host_buffers)
			{
				rsx_log.trace("Requested DMA passthrough for block 0x%x->0x%x but this was not possible.",
					base_address, base_address + expected_length - 1);
			}
		}

		if (allow_host_buffers)
		{
			block.reset(new dma_block_EXT());
		}
		else
		{
			block.reset(new dma_block());
		}

		block->init(*g_render_device, base_address, expected_length);
	}

	dma_mapping_handle map_dma(u32 local_address, u32 length)
	{
		// Not much contention expected here, avoid searching twice
		std::lock_guard lock(g_dma_mutex);

		const auto map_range = utils::address_range32::start_length(local_address, length);
		auto first_block = (local_address & s_dma_block_mask);

		if (auto found = g_dma_pool.find(first_block); found != g_dma_pool.end())
		{
			if (found->second->end() >= map_range.end)
			{
				return found->second->get(map_range);
			}
		}

		auto last_block = (map_range.end & s_dma_block_mask);
		if (first_block == last_block) [[likely]]
		{
			auto &block_info = g_dma_pool[first_block];
			ensure(!block_info);

			create_dma_block(block_info, first_block, s_dma_block_length);
			return block_info->get(map_range);
		}

		// Scan range for overlapping sections and update 'chains' accordingly
		for (auto block = first_block; block <= last_block; block += s_dma_block_length)
		{
			if (auto& entry = g_dma_pool[block])
			{
				first_block = std::min(first_block, entry->head()->start() & s_dma_block_mask);
				last_block = std::max(last_block, entry->end() & s_dma_block_mask);
			}
		}

		std::vector<std::unique_ptr<dma_block>> stale_references;
		dma_block* block_head = nullptr;

		for (auto block = first_block; block <= last_block; block += s_dma_block_length)
		{
			auto& entry = g_dma_pool[block];

			if (block == first_block)
			{
				if (entry)
				{
					// Then the references to this object do not go to the end of the list as will be done with this new allocation.
					// A dumb release is therefore safe...
					ensure(entry->end() < map_range.end);
					stale_references.push_back(std::move(entry));
				}

				auto required_size = (last_block - first_block + s_dma_block_length);
				create_dma_block(entry, block, required_size);
				block_head = entry->head();
			}
			else if (entry)
			{
				ensure((entry->end() & s_dma_block_mask) <= last_block);
				entry->set_parent(block_head);
			}
			else
			{
				entry.reset(new dma_block());
				entry->init(block_head, block, s_dma_block_length);
			}
		}

		// Check that all the math adds up...
		stale_references.clear();
		ensure(s_allocated_dma_pool_size == g_dma_pool.size() * s_dma_block_length);

		ensure(block_head);
		return block_head->get(map_range);
	}

	void unmap_dma(u32 local_address, u32 length)
	{
		std::lock_guard lock(g_dma_mutex);

		const u32 start = (local_address & s_dma_block_mask);
		const u32 end = utils::align(local_address + length, static_cast<u32>(s_dma_block_length));

		for (u32 block = start; block < end;)
		{
			if (auto found = g_dma_pool.find(block); found != g_dma_pool.end())
			{
				auto head = found->second->head();
				if (dynamic_cast<dma_block_EXT*>(head))
				{
					// Passthrough block. Must unmap from GPU
					const u32 start_block = head->start();
					const u32 last_block = head->start() + head->size();

					for (u32 block_ = start_block; block_ < last_block; block_ += s_dma_block_length)
					{
						g_dma_pool.erase(block_);
					}

					block = last_block;
					continue;
				}
			}

			block += s_dma_block_length;
		}

		ensure(s_allocated_dma_pool_size == g_dma_pool.size() * s_dma_block_length);
	}

	template<bool load>
	void sync_dma_impl(u32 local_address, u32 length, const dma_source_observer* observer = nullptr)
	{
		reader_lock lock(g_dma_mutex);

		const auto limit = local_address + length - 1;
		while (length)
		{
			u32 block = (local_address & s_dma_block_mask);
			if (auto found = g_dma_pool.find(block); found != g_dma_pool.end())
			{
				const auto sync_end = std::min(limit, found->second->end());
				const auto range = utils::address_range32::start_end(local_address, sync_end);

				if constexpr (load)
				{
					found->second->load(range);
				}
				else
				{
					found->second->flush(range, observer);
				}

				if (sync_end < limit) [[unlikely]]
				{
					// Technically legal but assuming a map->flush usage, this shouldnot happen
					// Optimizations could in theory batch together multiple transfers though
					rsx_log.error("Sink request spans multiple allocated blocks!");
					const auto write_end = (sync_end + 1u);
					const auto written = (write_end - local_address);
					length -= written;
					local_address = write_end;
					continue;
				}

				break;
			}
			else
			{
				rsx_log.error("Sync command on range not mapped!");
				return;
			}
		}
	}

	void dma_block::mark_gpu_written()
	{
		head()->m_gpu_written = true;
	}

	void mark_dma_gpu_written(u32 local_address, u32 length)
	{
		if (!compare_dma_enabled())
		{
			return;
		}
		std::lock_guard lock(g_dma_mutex);
		while (length)
		{
			auto found = g_dma_pool.find(local_address & s_dma_block_mask);
			ensure(found != g_dma_pool.end());
			auto block = found->second->head();
			block->mark_gpu_written();
			const u32 step = std::min(length, block->end() - local_address + 1u);
			local_address += step;
			length -= step;
		}
	}

	// Diagnostic only (RPCS3_VK_ROUNDTRIP_DIAG=1): for full 1280x720x4 buffers, remember the bytes the GPU
	// read back into guest memory and report how much of them the guest changed before they are uploaded again.
	namespace roundtrip_diag
	{
		static bool enabled()
		{
			static const bool value = []
			{
				const char* option = std::getenv("RPCS3_VK_ROUNDTRIP_DIAG");
				return option && option[0] == '1' && !option[1];
			}();
			return value;
		}

		constexpr u32 tracked_length = 1280 * 720 * 4;
		static std::mutex s_mutex;
		static std::unordered_map<u32, std::unique_ptr<u8[]>> s_shadow;
		static std::unordered_map<u32, u64> s_loads;
		static std::unordered_map<u32, u64> s_flushes;

		static void on_flush(u32 address, u32 length)
		{
			if (length != tracked_length) return;
			std::lock_guard lock(s_mutex);
			auto& shadow = s_shadow[address];
			if (!shadow) shadow = std::make_unique<u8[]>(tracked_length);
			std::memcpy(shadow.get(), vm::get_super_ptr<u8>(address), tracked_length);
			s_flushes[address]++;
		}

		static void on_load(u32 address, u32 length)
		{
			if (length != tracked_length) return;
			std::lock_guard lock(s_mutex);
			const auto found = s_shadow.find(address);
			if (found == s_shadow.end()) return;
			if (s_loads[address] == 240)
			{
				if (const char* dir = std::getenv("RPCS3_VK_ROUNDTRIP_DUMP_DIR"))
				{
					for (int pass = 0; pass < 2; pass++)
					{
						const std::string path = fmt::format("%s/rt-%08x-%s.raw", dir, address, pass ? "after" : "before");
						if (FILE* f = std::fopen(path.c_str(), "wb"))
						{
							std::fwrite(pass ? vm::get_super_ptr<u8>(address) : found->second.get(), 1, tracked_length, f);
							std::fclose(f);
						}
					}
				}
			}

			if (s_loads[address]++ % 60) return;

			const u8* now = vm::get_super_ptr<u8>(address);
			const u8* old = found->second.get();
			u32 rows = 0, first_row = 720, last_row = 0, min_col = 1280, max_col = 0;
			for (u32 y = 0; y < 720; y++)
			{
				const u8* a = now + y * 5120;
				const u8* b = old + y * 5120;
				if (!std::memcmp(a, b, 5120)) continue;
				rows++;
				first_row = std::min(first_row, y);
				last_row = y;
				for (u32 x = 0; x < 1280; x++)
				{
					if (std::memcmp(a + x * 4, b + x * 4, 4))
					{
						min_col = std::min(min_col, x);
						break;
					}
				}
				for (u32 x = 1280; x-- > 0;)
				{
					if (std::memcmp(a + x * 4, b + x * 4, 4))
					{
						max_col = std::max(max_col, x);
						break;
					}
				}
			}
			rsx_log.notice("Roundtrip diag 0x%x: %u/720 rows changed since readback (rows %u..%u, columns %u..%u), loads=%u flushes=%u",
				address, rows, first_row, last_row, min_col, max_col, s_loads[address], s_flushes[address]);
		}
	}

	void load_dma(u32 local_address, u32 length)
	{
		if (roundtrip_diag::enabled())
		{
			// Also dump any other large per-frame upload once (its 240th load)
			if (length >= 0x40000 && length != roundtrip_diag::tracked_length)
			{
				static std::mutex mutex;
				static std::unordered_map<u64, u32> seen;
				std::lock_guard lock(mutex);
				if (++seen[(u64{local_address} << 32) | length] == 240)
				{
					if (const char* dir = std::getenv("RPCS3_VK_ROUNDTRIP_DUMP_DIR"))
					{
						if (local_address == 0xcf800000)
						{
							// SSAO intermediates and both G-buffers as they are when the AO image is uploaded
							const std::pair<u32, u32> extra[]{{0x37b08000, 0x11a480}, {0x37400b80, 0x384000}, {0x37784b80, 0x384000}};
							for (const auto& [ea, size] : extra)
							{
								if (FILE* g = std::fopen(fmt::format("%s/mem-%08x-%u.raw", dir, ea, size).c_str(), "wb"))
								{
									std::fwrite(vm::get_super_ptr<u8>(ea), 1, size, g);
									std::fclose(g);
								}
							}
						}
						const std::string path = fmt::format("%s/load-%08x-%u.raw", dir, local_address, length);
						if (FILE* f = std::fopen(path.c_str(), "wb"))
						{
							std::fwrite(vm::get_super_ptr<u8>(local_address), 1, length, f);
							std::fclose(f);
						}
					}
				}
			}
		}

		if (roundtrip_diag::enabled()) roundtrip_diag::on_load(local_address, length);
		sync_dma_impl<true>(local_address, length);
	}

	void flush_dma(u32 local_address, u32 length, const dma_source_observer* observer)
	{
		sync_dma_impl<false>(local_address, length, observer);
		if (roundtrip_diag::enabled()) roundtrip_diag::on_flush(local_address, length);
	}

	void clear_dma_resources()
	{
		g_dma_pool.clear();
	}
}
