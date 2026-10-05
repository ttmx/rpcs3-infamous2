#pragma once

// Write tracking for guest memory, used to keep copies of static vertex and index data valid across frames.
//
// Linux 6.7+: the watched ranges are registered with userfaultfd in asynchronous write-protect mode. The kernel
// then resolves the first write to each protected page by itself (no signal, no handler thread) and
// PAGEMAP_SCAN reports the pages written since the previous scan and protects them again.
// Both views of guest memory that receive writes (vm::g_base_addr, vm::g_sudo_addr) are tracked.
//
// Scans happen lazily, on the first query after a guest/RSX synchronisation point (see geometry_sync.h).
// Everything here runs on the RSX thread.

#include "util/types.hpp"
#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_locking.h"
#include "geometry_sync.h"

#include <memory>

#if defined(__linux__) && __has_include(<linux/userfaultfd.h>)
#include <linux/userfaultfd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
#define RSX_WRITE_WATCH_SUPPORTED 1
#endif
#endif

namespace rsx::write_watch
{
	// Watch granularity. Pages are tracked individually; registration and scanning work on 1 MiB chunks.
	constexpr u32 chunk_shift = 20;
	constexpr u32 chunk_count = 1u << (32 - chunk_shift);
	constexpr u32 page_shift = 12;

	struct state_t
	{
		int uffd = -1;
		int pagemap = -1;
		bool failed = false;

		// Number of the last scan. A page written between scans s and s+1 gets page_epoch = s+1.
		u32 seq = 1;
		u64 synced_epoch = 0;
		std::unique_ptr<u32[]> page_epoch;
		u8 watched[chunk_count]{};
		u8 failures[chunk_count]{};
		u32 watched_chunks = 0;

		// Statistics
		u64 scans = 0;
		u64 scan_ns = 0;
		u64 written_pages = 0;
		u64 lost_chunks = 0;
	};

	inline state_t& state()
	{
		static state_t value;
		return value;
	}

#ifdef RSX_WRITE_WATCH_SUPPORTED
	inline bool available()
	{
		auto& s = state();
		if (s.failed) return false;
		if (s.uffd >= 0) return true;

		s.uffd = static_cast<int>(::syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY));
		if (s.uffd < 0)
		{
			s.failed = true;
			return false;
		}

		uffdio_api api{};
		api.api = UFFD_API;
		api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
		s.pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);

		if (::ioctl(s.uffd, UFFDIO_API, &api) != 0 || s.pagemap < 0)
		{
			::close(s.uffd);
			if (s.pagemap >= 0) ::close(s.pagemap);
			s.uffd = s.pagemap = -1;
			s.failed = true;
			return false;
		}

		s.page_epoch = std::make_unique<u32[]>(1u << (32 - page_shift));
		return true;
	}

	// Collect and re-protect the written pages of [begin, end) in one view. False if the range cannot be tracked.
	inline bool scan_view(const u8* view, u64 begin, u64 end, bool record)
	{
		auto& s = state();
		page_region regions[256];
		u64 start = reinterpret_cast<u64>(view) + begin;
		const u64 stop = reinterpret_cast<u64>(view) + end;

		while (start < stop)
		{
			pm_scan_arg arg{};
			arg.size = sizeof(arg);
			arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
			arg.start = start;
			arg.end = stop;
			arg.vec = reinterpret_cast<u64>(regions);
			arg.vec_len = 256;
			arg.category_mask = PAGE_IS_WRITTEN;
			arg.return_mask = PAGE_IS_WRITTEN;

			const long count = ::ioctl(s.pagemap, PAGEMAP_SCAN, &arg);
			if (count < 0)
			{
				return false;
			}

			if (record)
			{
				for (long i = 0; i < count; i++)
				{
					const u64 first = (regions[i].start - reinterpret_cast<u64>(view)) >> page_shift;
					const u64 last = (regions[i].end - reinterpret_cast<u64>(view)) >> page_shift;
					for (u64 page = first; page < last; page++)
					{
						s.page_epoch[page] = s.seq;
					}
					s.written_pages += last - first;
				}
			}

			if (!arg.walk_end || arg.walk_end >= stop || arg.walk_end <= start)
			{
				break;
			}

			start = arg.walk_end;
		}

		return true;
	}

	// Start tracking the chunks covering [address, address + size). Must be called before the bytes are copied.
	inline bool watch(u32 address, u32 size)
	{
		if (!size || !available()) return false;
		auto& s = state();

		const u32 first = address >> chunk_shift;
		const u32 last = static_cast<u32>((u64{address} + size - 1) >> chunk_shift);

		for (u32 chunk = first; chunk <= last; chunk++)
		{
			if (s.watched[chunk]) continue;
			if (s.failures[chunk] >= 3) return false;

			// Memory that is mapped at more than one guest address can be written through the other address
			for (u32 block = chunk << (chunk_shift - 16), end = block + (1u << (chunk_shift - 16)); block < end; block++)
			{
				if (vm::g_shmem[block].load()) return false;
			}

			const u64 begin = u64{chunk} << chunk_shift;
			const u64 length = u64{1} << chunk_shift;
			bool ok = true;

			for (const u8* view : { vm::g_base_addr, vm::g_sudo_addr })
			{
				uffdio_register reg{};
				reg.range.start = reinterpret_cast<u64>(view) + begin;
				reg.range.len = length;
				reg.mode = UFFDIO_REGISTER_MODE_WP;
				ok = ok && ::ioctl(s.uffd, UFFDIO_REGISTER, &reg) == 0;
			}

			// Protect every page of the chunk; what was written before this point is not of interest
			for (const u8* view : { vm::g_base_addr, vm::g_sudo_addr })
			{
				ok = ok && scan_view(view, begin, begin + length, false);
			}

			if (!ok)
			{
				s.failures[chunk]++;
				return false;
			}

			s.watched[chunk] = 1;
			s.watched_chunks++;
		}

		return true;
	}

	// Bring page_epoch up to date if the guest passed a synchronisation point since the last scan
	inline void sync()
	{
		auto& s = state();
		if (s.synced_epoch == geometry_sync::epoch) return;
		s.synced_epoch = geometry_sync::epoch;
		if (!s.watched_chunks) return;

		timespec t0{}, t1{};
		::clock_gettime(CLOCK_MONOTONIC, &t0);
		s.seq++;
		s.scans++;

		for (u32 chunk = 0; chunk < chunk_count;)
		{
			if (!s.watched[chunk])
			{
				chunk++;
				continue;
			}

			u32 run_end = chunk + 1;
			while (run_end < chunk_count && s.watched[run_end]) run_end++;

			const u64 begin = u64{chunk} << chunk_shift;
			const u64 end = u64{run_end} << chunk_shift;

			if (!scan_view(vm::g_base_addr, begin, end, true) || !scan_view(vm::g_sudo_addr, begin, end, true))
			{
				// Remapped or otherwise untrackable: everything in the run counts as written and is dropped
				for (u64 page = begin >> page_shift; page < (end >> page_shift); page++)
				{
					s.page_epoch[page] = s.seq;
				}

				for (u32 i = chunk; i < run_end; i++)
				{
					s.watched[i] = 0;
					s.watched_chunks--;
					s.lost_chunks++;
				}
			}

			chunk = run_end;
		}

		::clock_gettime(CLOCK_MONOTONIC, &t1);
		s.scan_ns += (t1.tv_sec - t0.tv_sec) * 1'000'000'000ll + (t1.tv_nsec - t0.tv_nsec);
	}
#else
	inline bool available() { return false; }
	inline bool watch(u32, u32) { return false; }
	inline void sync() {}
#endif

	inline u32 seq()
	{
		return state().seq;
	}

	// True if the range is tracked and no page of it was written after scan number `since`
	inline bool clean(u32 address, u32 size, u32 since)
	{
		const auto& s = state();
		const u32 last_chunk = static_cast<u32>((u64{address} + size - 1) >> chunk_shift);

		for (u32 chunk = address >> chunk_shift; chunk <= last_chunk; chunk++)
		{
			if (!s.watched[chunk]) return false;
		}

		const u32 last_page = static_cast<u32>((u64{address} + size - 1) >> page_shift);

		for (u32 page = address >> page_shift; page <= last_page; page++)
		{
			if (s.page_epoch[page] > since) return false;
		}

		return true;
	}
}
