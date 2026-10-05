#pragma once

// Reuse of descriptor sets with identical contents (live control 12, glsl::descriptor_table_t::commit).
// Shared state: the image view and sampler handles that were destroyed. A set written with such a handle must not be
// matched by a later object that receives the same handle value. Buffers, buffer views and images are identified by
// their unique ids instead.
#include <atomic>
#include <cstdint>
#include <mutex>

namespace vk::descriptor_reuse
{
	inline constexpr std::uint64_t log_size = 1024;
	inline std::mutex retire_lock;
	inline std::atomic<std::uint64_t> retire_count{0};
	inline std::uint64_t retire_log[log_size]{};

	// Statistics since the last report
	inline std::uint64_t hits = 0, misses = 0, unkeyed = 0, syncs = 0, dropped = 0, pools_retired = 0, fallbacks = 0;
	inline std::atomic<std::uint64_t> retired_views{0}, retired_samplers{0};

	// One of 64 classes for a handle value
	inline std::uint64_t handle_bit(std::uint64_t handle)
	{
		return 1ull << ((handle * 0x9e3779b97f4a7c15ull) >> 58);
	}

	inline void retire_handle(std::uint64_t handle)
	{
		std::lock_guard lock(retire_lock);
		const auto count = retire_count.load(std::memory_order_relaxed);
		retire_log[count % log_size] = handle;
		retire_count.store(count + 1, std::memory_order_release);
	}

	// The handles destroyed since 'seen' (up to 'capacity'; false if there were more or the log has wrapped: treat every
	// handle as destroyed), their classes, and 'seen' brought up to date
	inline bool retired_since(std::uint64_t& seen, std::uint64_t* handles, std::uint32_t capacity, std::uint32_t& count_out, std::uint64_t& classes)
	{
		std::lock_guard lock(retire_lock);
		const auto count = retire_count.load(std::memory_order_relaxed);
		const bool listed = count - seen <= capacity && count - seen <= log_size;
		classes = listed ? 0 : ~0ull;
		count_out = 0;

		for (auto i = seen; listed && i != count; i++)
		{
			handles[count_out++] = retire_log[i % log_size];
			classes |= handle_bit(handles[count_out - 1]);
		}

		seen = count;
		return listed;
	}
}
