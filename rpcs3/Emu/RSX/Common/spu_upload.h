#pragma once
// Vertex data written by SPU jobs goes to a host GPU buffer at the moment the job writes it to guest memory
// (RPCS3_SPU_VERTEX_UPLOAD=1). God of War III builds every draw's vertices in an SPU job each frame, about 20 MB a
// frame; the render thread used to copy all of it again from guest memory, cold, into its upload ring. Here the SPU
// thread's PUT also appends the bytes (still in its cache) to a heap that the vertex shader reads, and publishes which
// guest range they are. A draw whose single vertex block lies inside one published range binds the heap at that offset
// and copies nothing.
//
// What is tracked: every SPU PUT, list PUT and atomic line store either publishes its range (a known job's PUT of
// 256 bytes and more) or clears it. What is
// not: writes by PPU threads. A game that patches a job's vertex output from the PPU between the job's PUT and the
// draw would draw the unpatched bytes; live control 19 = 2 compares every range used with guest memory and counts
// differences (RPCS3_SPU_VERTEX_UPLOAD_STATS=1 prints the counters).
//
// Lifetime: the heap is a ring written in 1 MB chunks, one open chunk per SPU thread. A range is used only while it is
// younger than half the ring, which at God of War III's rate is more than a hundred milliseconds; draws that refer to
// it have long been executed by the host GPU when the ring comes around.
#include "util/types.hpp"
#include "Emu/RSX/VK/VKLiveCtl.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace rsx::spu_upload
{
	constexpr u64 chunk_size = 0x100000;
	constexpr u32 granule_shift = 8;
	constexpr u32 minimum_bytes = 256;
	constexpr u32 record_count = 1u << 20;
	constexpr u32 id_block = 1024; // a divisor of 2^32, so that a block never wraps inside

	struct record_t
	{
		std::atomic<u32> id;
		u32 guest;                  // first guest address
		std::atomic<u32> guest_end; // one past the last; grows while the same thread appends adjoining PUTs
		u64 position;               // ring position of the first byte (never wraps; the offset is position % size)
	};

	struct state_t
	{
		std::atomic<u8*> mapped{nullptr}; // the heap, mapped; null until the renderer has made it
		std::atomic<bool> wanted{false};  // a job whose output is worth publishing has run: the renderer makes the heap
		u64 size = 0;                     // bytes, a multiple of the chunk size
		std::atomic<u64> position{0};     // bytes handed out in chunks so far
		std::atomic<u64>* table = nullptr; // by guest address >> granule_shift: the ids of up to two ranges with bytes there, 0 = none
		record_t* records = nullptr;
		std::atomic<u32> next_id{1};

		// Index data (see write_twin): a second heap with every byte pair of the first swapped, and by guest granule
		// the smallest and largest 16-bit value in it (low and high half; low above high = not known)
		std::atomic<u8*> twin{nullptr};
		u32* ranges = nullptr;
		u64 index_hits = 0, index_bytes = 0, index_differing = 0;

		// Counters (render thread only, except the first two)
		std::atomic<u64> published{0}, published_bytes{0};
		u64 appended = 0, appended_bytes = 0;
		u64 hits = 0, hit_bytes = 0, no_record = 0, partial = 0, old = 0, multi_block = 0, miss_bytes = 0, verified = 0, differing = 0;
	};

	inline state_t g_state;

	inline bool enabled()
	{
		static const bool value = []
		{
			const char* option = std::getenv("RPCS3_SPU_VERTEX_UPLOAD");
			return option && option[0] == '1' && !option[1];
		}();
		return value;
	}

	// Only the output of jobs known to write vertex data that nothing else touches is published: God of War III's
	// geometry job (the program SPUNativeGeometry.hpp has kernels for, recognised by the same instructions). In
	// Ratchet & Clank, published at large, 5,379 of 3.4 million ranges differed from guest memory at the draw.
	inline bool from_known_job(const u8* ls)
	{
		static constexpr u8 skinning_loop[8]{0x0f, 0x60, 0xdf, 0x9e, 0x04, 0x00, 0x04, 0xb2};
		return !std::memcmp(ls + 0x10d74, skinning_loop, 8);
	}

	inline bool stats_wanted()
	{
		static const bool wanted = std::getenv("RPCS3_SPU_VERTEX_UPLOAD_STATS") != nullptr;
		return wanted;
	}

	// Live control 19: 1 = draws do not use the heap, 2 = every use is compared with guest memory, 3 = PUTs do not publish either
	inline u64 mode()
	{
		return vk::live_ctl::get(19);
	}

	// Called once by the renderer with its mapped heap
	inline void attach(u8* mapped, u64 size)
	{
		auto& s = g_state;

		if (!s.table)
		{
			// Zero pages until touched: 128 MB of address space for the table, 24 MB for the records
			s.table = static_cast<std::atomic<u64>*>(std::calloc(usz{1} << (32 - granule_shift), sizeof(u64)));
			s.records = static_cast<record_t*>(std::calloc(record_count, sizeof(record_t)));
		}

		s.size = size;
		s.mapped = mapped;
	}

	inline void detach()
	{
		g_state.twin = nullptr;
		g_state.mapped = nullptr;
	}

	// RPCS3_SPU_INDEX_UPLOAD=1: the jobs' output is also kept as 16-bit index data
	inline bool indices_enabled()
	{
		static const bool value = []
		{
			const char* option = std::getenv("RPCS3_SPU_INDEX_UPLOAD");
			return option && option[0] == '1' && !option[1];
		}();
		return value;
	}

	constexpr u32 unknown_range = 0x0000ffff;

	inline void attach_twin(u8* mapped)
	{
		auto& s = g_state;
		if (!s.ranges) s.ranges = static_cast<u32*>(std::calloc(usz{1} << (32 - granule_shift), sizeof(u32)));
		s.twin = mapped;
	}

	// The job writes each draw's index list with the same PUTs as its vertices, big-endian, and the render thread
	// converted every list again (byte swap and a search for the smallest and largest index, 9% of its time in God
	// of War III). Nothing tells an index list from vertex data at the PUT, so all of it is written a second time
	// with the bytes of every pair swapped, by the SPU thread that has the data in its cache, together with the
	// extremes of each whole 256-byte granule. A draw whose index list lies in one published range binds this heap.
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	__attribute__((target("ssse3,sse4.1")))
#endif
	inline void write_twin(u8* dst, const u8* src, u32 guest, u32 bytes, u32* ranges)
	{
		const u32 end = guest + bytes;

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
		if (!((guest | bytes | reinterpret_cast<uptr>(dst)) & 15))
		{
			const __m128i swap = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);

			for (u32 addr = guest; addr < end;)
			{
				const u32 first = addr, last = std::min<u32>(end, (addr | 255) + 1);
				__m128i low = _mm_set1_epi16(-1), high = _mm_setzero_si128();

				for (; addr < last; addr += 16, src += 16, dst += 16)
				{
					const __m128i v = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)), swap);
					low = _mm_min_epu16(low, v);
					high = _mm_max_epu16(high, v);
					_mm_stream_si128(reinterpret_cast<__m128i*>(dst), v);
				}

				if (!(first & 255) && last - first == 256)
				{
					const u32 min = static_cast<u16>(_mm_cvtsi128_si32(_mm_minpos_epu16(low)));
					const u32 max = static_cast<u16>(~_mm_cvtsi128_si32(_mm_minpos_epu16(_mm_xor_si128(high, _mm_set1_epi16(-1)))));
					ranges[first >> granule_shift] = min | max << 16;
				}
				else
				{
					ranges[first >> granule_shift] = unknown_range;
				}
			}

			return;
		}
#endif

		for (u32 i = 0; i + 1 < bytes; i += 2)
		{
			dst[i] = src[i + 1];
			dst[i + 1] = src[i];
		}

		for (u32 g = guest >> granule_shift; g <= (end - 1) >> granule_shift; g++)
		{
			ranges[g] = unknown_range;
		}
	}

	// A write to guest memory that does not publish: whatever was published for these bytes is no longer true
	inline void invalidate(u32 guest, u32 bytes)
	{
		if (!g_state.mapped.load(std::memory_order_relaxed) || !bytes) [[likely]] return;

		auto* table = g_state.table;
		for (u32 g = guest >> granule_shift, last = (guest + bytes - 1) >> granule_shift; g <= last; g++)
		{
			if (table[g].load(std::memory_order_relaxed)) table[g].store(0);
		}
	}

	// A granule that a range covers only in part keeps the id of another range there if that one has other bytes of
	// it (two jobs' outputs meet inside a granule all the time). Never more than two: a third pushes one out, and a
	// range that is not listed in every granule it touches is not found.
	inline void share_granule(std::atomic<u64>& entry, u32 id, u32 first, u32 end)
	{
		const auto keeps = [&](u32 other)
		{
			if (!other || other == id) return false;
			const record_t& rec = g_state.records[other % record_count];
			const u32 other_first = rec.guest, other_end = rec.guest_end.load(std::memory_order_acquire);
			return rec.id.load(std::memory_order_acquire) == other && (other_end <= first || other_first >= end);
		};

		for (u64 old = entry.load();;)
		{
			const u32 a = static_cast<u32>(old), b = static_cast<u32>(old >> 32);
			const u32 kept = keeps(a) ? a : keeps(b) ? b : 0;

			if (entry.compare_exchange_weak(old, u64{id} | u64{kept} << 32))
			{
				return;
			}
		}
	}

	// One SPU thread's open chunk
	struct arena_t
	{
		u64 position = 0, end = 0;
		record_t* current = nullptr;
		u32 next_id = 0, last_id = 0;
	};

	// An SPU PUT of data that is about to be (or has just been) written to guest memory
	template <typename Copy>
	inline void publish(u32 guest, const void* data, u32 bytes, Copy&& copy)
	{
		u8* const mapped = g_state.mapped.load(std::memory_order_acquire);

		if (!mapped) [[unlikely]]
		{
			g_state.wanted.store(true, std::memory_order_relaxed);
			return;
		}

		if (bytes < minimum_bytes || mode() == 3)
		{
			invalidate(guest, bytes);
			return;
		}

		vk::live_ctl::probe_delay(1);

		static thread_local arena_t arena;

		if (arena.position + bytes > arena.end)
		{
			arena.position = g_state.position.fetch_add(chunk_size);
			arena.end = arena.position + chunk_size;
			arena.current = nullptr;
		}

		copy(mapped + arena.position % g_state.size, data, bytes);

		if (u8* const twin = g_state.twin.load(std::memory_order_relaxed))
		{
			write_twin(twin + arena.position % g_state.size, static_cast<const u8*>(data), guest, bytes, g_state.ranges);
		}

		record_t* rec = arena.current;
		u32 id;

		if (rec && rec->guest_end.load(std::memory_order_relaxed) == guest && rec->position + (guest - rec->guest) == arena.position)
		{
			// Adjoins what this thread wrote last, in guest memory and in the heap: one longer range
			id = rec->id.load(std::memory_order_relaxed);
		}
		else
		{
			// Ids are taken from the shared counter a block at a time
			if (arena.next_id == arena.last_id)
			{
				arena.next_id = g_state.next_id.fetch_add(id_block);
				arena.last_id = arena.next_id + id_block;
			}

			id = arena.next_id++;
			if (!id) id = arena.next_id++;

			rec = &g_state.records[id % record_count];
			rec->id.store(0, std::memory_order_release);
			rec->guest = guest;
			rec->position = arena.position;
			arena.current = rec;
		}

		// Plain stores: the render thread reads the end and the id after the table entry that names the record
		rec->guest_end.store(guest + bytes, std::memory_order_release);
		rec->id.store(id, std::memory_order_release);

		auto* table = g_state.table;
		const u32 end = guest + bytes, granule = 1u << granule_shift;
		u32 g = guest >> granule_shift, last = (end - 1) >> granule_shift;

		if (guest & (granule - 1))
		{
			share_granule(table[g], id, guest, end);
			g++;
		}

		if ((end & (granule - 1)) && last >= g)
		{
			share_granule(table[last], id, guest, end);
			last--;
		}

		for (; g <= last && g; g++)
		{
			table[g].store(u64{id} | u64{id} << 32, std::memory_order_relaxed);
		}

		arena.position += bytes;

		if (stats_wanted()) [[unlikely]]
		{
			g_state.published.fetch_add(1, std::memory_order_relaxed);
			g_state.published_bytes.fetch_add(bytes, std::memory_order_relaxed);
		}
	}

	// Bytes that no SPU wrote (a static stream next to a job's stream in one draw), put into the heap by the caller's
	// thread so that all of the draw's blocks are in it. Not published: the next draw asks again.
	template <typename Copy>
	inline u64 append(const void* data, u32 bytes, Copy&& copy)
	{
		u8* const mapped = g_state.mapped.load(std::memory_order_acquire);
		if (!mapped || bytes > chunk_size) return umax;

		static thread_local arena_t arena;

		if (arena.position + bytes > arena.end)
		{
			arena.position = g_state.position.fetch_add(chunk_size);
			arena.end = arena.position + chunk_size;
		}

		const u64 offset = arena.position % g_state.size;
		copy(mapped + offset, data, bytes);
		arena.position += (bytes + 63) & ~63u;
		return offset;
	}

	// Render thread: the heap offset of guest bytes [guest, guest + bytes) if one published range holds all of them
	inline u64 find(u32 guest, u32 bytes)
	{
		auto& s = g_state;
		const auto* table = s.table;
		const u32 first = guest >> granule_shift, last = (guest + bytes - 1) >> granule_shift;
		const u64 entry = table[first].load(std::memory_order_acquire);

		if (!entry)
		{
			s.no_record++;
			return umax;
		}

		for (const u32 id : {static_cast<u32>(entry), static_cast<u32>(entry >> 32)})
		{
			if (!id) continue;

			const record_t& rec = s.records[id % record_count];

			if (rec.id.load(std::memory_order_acquire) != id || guest < rec.guest || u64{guest} + bytes > rec.guest_end.load(std::memory_order_acquire))
			{
				continue;
			}

			bool listed = true;

			for (u32 g = first + 1; g <= last && listed; g++)
			{
				const u64 e = table[g].load(std::memory_order_relaxed);
				listed = static_cast<u32>(e) == id || static_cast<u32>(e >> 32) == id;
			}

			if (!listed) continue;

			const u64 position = rec.position + (guest - rec.guest);

			if (s.position.load(std::memory_order_relaxed) - position >= s.size / 2)
			{
				s.old++;
				return umax;
			}

			// The record may have been taken for another range meanwhile
			if (rec.id.load(std::memory_order_acquire) != id) continue;

			return position % s.size;
		}

		s.partial++;
		return umax;
	}

	// Render thread: the offset in the twin heap of the 16-bit index list at guest bytes [guest, guest + bytes) and
	// its smallest and largest index, if one published range holds the list
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	__attribute__((target("sse4.1")))
#endif
	inline u64 find_indices(u32 guest, u32 bytes, u32& min_index, u32& max_index)
	{
		auto& s = g_state;
		const u8* const twin = s.twin.load(std::memory_order_relaxed);

		if (!twin || (guest & 1) || bytes < 2)
		{
			return umax;
		}

		const u64 offset = find(guest, bytes);

		if (offset == umax)
		{
			return umax;
		}

		const u8* const data = twin + offset;
		u32 low = 0xffff, high = 0;

		for (u32 addr = guest, end = guest + bytes; addr < end;)
		{
			const u32 last = std::min<u32>(end, (addr | 255) + 1);
			const u32 range = s.ranges[addr >> granule_shift];

			if (!(addr & 255) && last - addr == 256 && (range & 0xffff) <= range >> 16)
			{
				low = std::min<u32>(low, range & 0xffff);
				high = std::max<u32>(high, range >> 16);
				addr = last;
				continue;
			}

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
			if (last - addr >= 16)
			{
				__m128i lows = _mm_set1_epi16(-1), highs = _mm_setzero_si128();

				for (; addr + 16 <= last; addr += 16)
				{
					const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + (addr - guest)));
					lows = _mm_min_epu16(lows, v);
					highs = _mm_max_epu16(highs, v);
				}

				low = std::min<u32>(low, static_cast<u16>(_mm_cvtsi128_si32(_mm_minpos_epu16(lows))));
				high = std::max<u32>(high, static_cast<u16>(~_mm_cvtsi128_si32(_mm_minpos_epu16(_mm_xor_si128(highs, _mm_set1_epi16(-1))))));
			}
#endif

			for (; addr < last; addr += 2)
			{
				u16 index;
				std::memcpy(&index, data + (addr - guest), 2);
				low = std::min<u32>(low, index);
				high = std::max<u32>(high, index);
			}
		}

		min_index = low;
		max_index = high;
		s.index_hits++;
		s.index_bytes += bytes;
		return offset;
	}

	inline void print_stats()
	{
		if (!stats_wanted()) return;

		auto& s = g_state;
		std::fprintf(stderr, "SPU vertex upload: published %llu (%.1f MB); draws: %llu from the heap (%.1f MB, of them %.1f MB in %llu blocks put there by the render thread), copied: %llu no range, %llu not inside one range, %llu too old, %llu too many blocks (%.1f MB); compared %llu, differing %llu\n",
			static_cast<unsigned long long>(s.published.exchange(0)), s.published_bytes.exchange(0) / 1048576., static_cast<unsigned long long>(s.hits), s.hit_bytes / 1048576., s.appended_bytes / 1048576., static_cast<unsigned long long>(s.appended),
			static_cast<unsigned long long>(s.no_record), static_cast<unsigned long long>(s.partial), static_cast<unsigned long long>(s.old), static_cast<unsigned long long>(s.multi_block), s.miss_bytes / 1048576.,
			static_cast<unsigned long long>(s.verified), static_cast<unsigned long long>(s.differing));
		std::fprintf(stderr, "SPU index upload: %llu lists from the heap (%.1f MB), compared and differing %llu\n",
			static_cast<unsigned long long>(s.index_hits), s.index_bytes / 1048576., static_cast<unsigned long long>(s.index_differing));
		s.index_hits = s.index_bytes = s.index_differing = 0;
		s.appended = s.appended_bytes = 0;
		s.hits = s.hit_bytes = s.no_record = s.partial = s.old = s.multi_block = s.miss_bytes = s.verified = s.differing = 0;
	}
}
