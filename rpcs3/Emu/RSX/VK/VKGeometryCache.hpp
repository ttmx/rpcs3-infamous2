#pragma once

// Cross-frame cache of static vertex and index data.
//
// Stock behaviour copies every draw's vertex ranges and converted indices from guest memory into ring buffers,
// every frame. Here a source that is requested again in a later frame is copied once more into a persistent
// buffer ("promoted") and its guest pages are write-tracked (Common/write_watch.h). Later draws use the
// persistent copy for as long as none of those pages was written. Sources that keep changing are left to the
// stock path.
//
// Mode (live control 8, initial value from RPCS3_VK_GEOMETRY_CACHE): 0 = off, 1 = on,
// 2 = on, and every reuse is checked against a hash of the current guest bytes (diagnostic).

#include "VKLiveCtl.hpp"
#include "VKResourceManager.h"
#include "vkutils/buffer_object.h"
#include "vkutils/device.h"
#include "../Common/write_watch.h"
#include "util/asm.hpp"

#include <memory>

namespace vk
{
	struct geometry_cache
	{
		static constexpr u32 max_ranges = 2;

		struct range_t
		{
			u32 address = 0;
			u32 length = 0;

			bool operator==(const range_t&) const = default;
		};

		// What the cached bytes are a function of: the ordered guest ranges, plus the conversion parameters for indices
		struct key_t
		{
			range_t ranges[max_ranges]{};
			u32 range_count = 0;
			u32 params = 0;
			u32 restart_index = 0;

			bool operator==(const key_t&) const = default;

			u64 hash() const
			{
				u64 h = 0x9e3779b97f4a7c15ull ^ (u64{params} << 32 | restart_index);
				for (u32 i = 0; i < range_count; i++)
				{
					h = (h ^ (u64{ranges[i].length} << 32 | ranges[i].address)) * 0x9fb21c651e98df25ull;
					h ^= h >> 31;
				}
				h *= 0xff51afd7ed558ccdull;
				return h ^ (h >> 32);
			}
		};

		// One cache line: everything a reuse needs
		struct alignas(64) slot_t
		{
			key_t key;

			u32 last_frame = 0;          // Last frame this source was requested in

			// Persistent copy
			u32 heap_generation = 0;     // 0 = none
			u32 seq = 0;                 // Write-watch scan number at copy time
			u32 offset = 0;

			// Index conversion results
			u32 min_index = 0;
			u32 max_index = 0;
			u32 index_count = 0;
		};

		// Rarely needed state of a slot
		struct cold_t
		{
			u64 content = 0;             // Hash of the guest bytes at copy time
			u32 first_frame = 0;
			u32 invalidations = 0;
			u32 volatile_until = 0;      // Not promoted again before this frame

			// Ring copy, valid within its frame (same trust as the stock weak vertex cache)
			u32 ring_frame = 0;
			u32 ring_generation = 0;
			u32 ring_offset = 0;
		};

		static_assert(sizeof(slot_t) == 64);

		struct table_t
		{
			static constexpr u32 slot_count = 1u << 15;
			static constexpr u32 probe_limit = 24;
			static constexpr u32 idle_frames = 300;

			// Probing only reads the tags (upper hash bits, 0 = free); a slot is touched when its tag matches
			std::unique_ptr<u32[]> tags;
			std::unique_ptr<slot_t[]> entries;
			std::unique_ptr<cold_t[]> cold_entries;

			cold_t& cold(const slot_t* slot)
			{
				return cold_entries[slot - entries.get()];
			}

			// Returns the slot of the key (found = true), or a slot that may be overwritten, or null
			slot_t* probe(const key_t& key, u32 frame, bool& found)
			{
				found = false;

				if (!entries)
				{
					tags = std::make_unique<u32[]>(slot_count);
					entries = std::make_unique<slot_t[]>(slot_count);
					cold_entries = std::make_unique<cold_t[]>(slot_count);
				}

				const u64 hash = key.hash();
				const u32 tag = static_cast<u32>(hash >> 32) | 1;
				const u32 home = static_cast<u32>(hash) & (slot_count - 1);

				for (u32 index = home, checked = 0; checked < probe_limit; checked++, index = (index + 1) & (slot_count - 1))
				{
					if (tags[index] == tag && entries[index].key == key)
					{
						found = true;
						return &entries[index];
					}

					if (!tags[index])
					{
						tags[index] = tag;
						return &entries[index];
					}
				}

				// Chain is full: take over a source that has not been requested for a while
				for (u32 index = home, checked = 0; checked < probe_limit; checked++, index = (index + 1) & (slot_count - 1))
				{
					if (frame - entries[index].last_frame > idle_frames)
					{
						tags[index] = tag;
						return &entries[index];
					}
				}

				return nullptr;
			}
		};

		// Persistent storage. Space is only reclaimed by replacing the whole buffer, because frames still in
		// flight may read any part of it; the old buffer goes through the deferred resource disposal.
		struct heap_t
		{
			std::unique_ptr<vk::buffer> buffer;
			std::unique_ptr<vk::buffer_view> view;
			u8* mapped = nullptr;
			u32 size = 0;
			u32 used = 0;
			u32 generation = 0;
			u32 created_frame = 0;
			VkBufferUsageFlags usage = 0;

			void create(u32 bytes, VkBufferUsageFlags usage_flags, u32 frame)
			{
				destroy(true);

				const auto& dev = *vk::get_current_renderer();
				const auto& memory_map = dev.get_memory_mapping();
				buffer = std::make_unique<vk::buffer>(dev, bytes, memory_map.host_visible_coherent,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, usage_flags, 0, VMM_ALLOCATION_POOL_SYSTEM);
				mapped = static_cast<u8*>(buffer->map(0, bytes));

				if (usage_flags & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)
				{
					view = std::make_unique<vk::buffer_view>(dev, buffer->value, VK_FORMAT_R8_UINT, 0, bytes);
				}

				size = bytes;
				used = 0;
				usage = usage_flags;
				created_frame = frame;
				generation++;
			}

			void destroy(bool deferred)
			{
				if (!buffer) return;

				buffer->unmap();
				mapped = nullptr;

				if (deferred)
				{
					if (view) vk::get_resource_manager()->dispose(view);
					vk::get_resource_manager()->dispose(buffer);
				}

				view.reset();
				buffer.reset();
			}

			// Offset of the new block, or umax when the heap is full
			u32 alloc(u32 bytes, u32 alignment)
			{
				const u32 start = utils::align(used, alignment);
				if (!buffer || bytes > size || start > size - bytes) return umax;
				used = start + bytes;
				return start;
			}
		};

		// Draws repeat in nearly the same order every frame. The slots (and write-watch entries) that the lookups of the
		// previous frame touched are prefetched a few lookups ahead; a wrong guess costs nothing but the prefetch.
		struct sequence_t
		{
			static constexpr u32 capacity = 1u << 15;
			static constexpr u32 distance = 6;

			std::unique_ptr<const void*[]> current;
			std::unique_ptr<const void*[]> previous;
			u32 count = 0;
			u32 previous_count = 0;

			void note(const void* slot, u32 address)
			{
				if (!current)
				{
					current = std::make_unique<const void*[]>(capacity * 2);
					previous = std::make_unique<const void*[]>(capacity * 2);
				}

				if (count + distance < previous_count)
				{
					utils::prefetch_read(previous[(count + distance) * 2]);
					utils::prefetch_read(previous[(count + distance) * 2 + 1]);
				}

				if (count < capacity)
				{
					current[count * 2] = slot;
					current[count * 2 + 1] = rsx::write_watch::state().page_epoch ? &rsx::write_watch::state().page_epoch[address >> rsx::write_watch::page_shift] : nullptr;
					count++;
				}
			}

			void end_frame()
			{
				std::swap(current, previous);
				previous_count = count;
				count = 0;
			}
		} sequence;

		table_t vertices;
		table_t indices;
		heap_t vertex_heap;
		heap_t index_heap;

		// Statistics since the last report
		struct stats_t
		{
			u64 vertex_requests = 0, vertex_static_hits = 0, vertex_ring_hits = 0, vertex_promotions = 0, vertex_invalidations = 0;
			u64 index_requests = 0, index_static_hits = 0, index_promotions = 0, index_invalidations = 0;
			u64 bytes_reused = 0, heap_resets = 0, verify_checks = 0, verify_mismatches = 0, dynamic_blocks = 0;
		} stats;

		u32 report_frame = 0;

		// A game whose geometry is written again every frame (God of War III: the SPUs skin everything) gets nothing
		// from the cache and pays for the lookups and the page scans. While almost no request is answered from it the
		// cache is suspended; it is tried again after a while, in case another part of the game is different.
		static inline bool suspended = false;
		static inline u32 resume_frame = 0;

		static u32 mode()
		{
			return suspended ? 0 : static_cast<u32>(vk::live_ctl::get(8));
		}

		// A full heap is replaced, but not more often than every few seconds
		static bool refill(heap_t& heap, u32 bytes, VkBufferUsageFlags usage, u32 frame)
		{
			if (heap.buffer && frame - heap.created_frame < 300) return false;
			heap.create(bytes, usage, frame);
			return true;
		}

		static bool is_clean(const slot_t& slot)
		{
			for (u32 i = 0; i < slot.key.range_count; i++)
			{
				if (!rsx::write_watch::clean(slot.key.ranges[i].address, slot.key.ranges[i].length, slot.seq)) return false;
			}
			return true;
		}

		static bool watch(const key_t& key)
		{
			for (u32 i = 0; i < key.range_count; i++)
			{
				if (!rsx::write_watch::watch(key.ranges[i].address, key.ranges[i].length)) return false;
			}
			return true;
		}

		static u64 content_hash(const key_t& key)
		{
			u64 h = 0;
			for (u32 i = 0; i < key.range_count; i++)
			{
				const auto* p = vm::get_super_ptr<const u8>(key.ranges[i].address);
				u32 length = key.ranges[i].length;
				h = (h ^ length) * 0x9fb21c651e98df25ull;
				for (; length >= 8; length -= 8, p += 8)
				{
					u64 v;
					std::memcpy(&v, p, 8);
					h = (h ^ v) * 0x9fb21c651e98df25ull;
					h ^= h >> 32;
				}
				u64 tail = 0;
				std::memcpy(&tail, p, length);
				h = (h ^ tail) * 0x9fb21c651e98df25ull;
				h ^= h >> 29;
			}
			return h;
		}

		// Memory that keeps being rewritten (animated geometry in recycled buffers) is not worth copies and tracking.
		// Invalidations are counted per 64 KiB of guest memory; a busy block is left to the stock path for a while.
		struct block_t
		{
			u32 dynamic_until = 0;
			u32 window_start = 0;
			u32 invalidations = 0;
		};

		static constexpr u32 churn_limit = 8;
		static constexpr u32 churn_window = 600;
		static constexpr u32 dynamic_frames = 1800;

		std::unique_ptr<block_t[]> blocks;

		void invalidated(const key_t& key, u32 frame)
		{
			if (!blocks) blocks = std::make_unique<block_t[]>(65536);

			for (u32 i = 0; i < key.range_count; i++)
			{
				auto& block = blocks[key.ranges[i].address >> 16];

				if (frame - block.window_start > churn_window)
				{
					block.window_start = frame;
					block.invalidations = 0;
				}

				if (++block.invalidations >= churn_limit)
				{
					block.dynamic_until = frame + dynamic_frames;
					block.invalidations = 0;
					stats.dynamic_blocks++;
				}
			}
		}

		bool promotable(const key_t& key, u32 frame) const
		{
			if (!blocks) return true;

			for (u32 i = 0; i < key.range_count; i++)
			{
				if (frame < blocks[key.ranges[i].address >> 16].dynamic_until) return false;
			}

			return true;
		}

		// Diagnostic (mode 2): a reused copy whose source hash no longer matches. Reported once the next scan shows
		// whether the write was tracked (it arrived after the last synchronisation point) or not seen at all.
		struct suspect_t
		{
			key_t key;
			u32 seq = 0;
			u32 frame = 0;
			u32 created_seq = 0;
			u32 first_frame = 0;
			u32 differing_bytes = 0;
			u32 first_difference = 0;
			bool vertex = false;
			bool rehash_equal = false;
			u32 last_kind = 0;
			u32 draws_since_point = 0;
			u32 params = 0;
		};

		suspect_t suspects[16]{};
		u32 suspect_count = 0;

		void suspect(const slot_t& slot, const cold_t& cold, bool vertex, u32 frame, const u8* copy)
		{
			if (suspect_count >= 16) return;
			auto& entry = suspects[suspect_count++];
			entry.key = slot.key;
			entry.seq = rsx::write_watch::seq();
			entry.frame = frame;
			entry.created_seq = slot.seq;
			entry.first_frame = cold.first_frame;
			entry.vertex = vertex;
			entry.last_kind = rsx::geometry_sync::last_kind;
			entry.draws_since_point = static_cast<u32>(rsx::geometry_sync::draws - rsx::geometry_sync::draws_at_last_point);
			entry.params = slot.key.params;
			entry.differing_bytes = 0;
			entry.first_difference = umax;

			if (copy)
			{
				for (u32 i = 0; i < slot.key.range_count; i++)
				{
					const auto* source = vm::get_super_ptr<const u8>(slot.key.ranges[i].address);
					for (u32 n = 0; n < slot.key.ranges[i].length; n++)
					{
						if (source[n] != copy[n])
						{
							if (!entry.differing_bytes++) entry.first_difference = slot.key.ranges[i].address + n;
						}
					}
					copy += slot.key.ranges[i].length;
				}
			}

			entry.rehash_equal = content_hash(slot.key) == cold.content;
		}

		template <typename Log>
		void report_suspects(Log&& log)
		{
			u32 kept = 0;
			for (u32 i = 0; i < suspect_count; i++)
			{
				auto& entry = suspects[i];
				if (rsx::write_watch::seq() == entry.seq)
				{
					suspects[kept++] = entry;
					continue;
				}

				bool tracked = false;
				for (u32 r = 0; r < entry.key.range_count; r++)
				{
					tracked |= !rsx::write_watch::clean(entry.key.ranges[r].address, entry.key.ranges[r].length, entry.seq);
				}

				log(entry, tracked);
			}
			suspect_count = kept;
		}

		void destroy()
		{
			vertex_heap.destroy(false);
			index_heap.destroy(false);
		}
	};
}
