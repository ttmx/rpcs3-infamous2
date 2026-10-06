#pragma once

#include "util/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>

// Diagnostic only (RPCS3_ROUNDTRIP_SURVEY=<file>): which guest memory the renderer reads back from the GPU, which it
// uploads as textures, and which SPU programs transfer to or from it. Written as one table every five seconds; a
// region that shows up in all three is an image the game renders, works on with the SPUs and samples again.
namespace roundtrip_survey
{
	inline const char* path()
	{
		static const char* value = std::getenv("RPCS3_ROUNDTRIP_SURVEY");
		return value && *value ? value : nullptr;
	}

	inline bool enabled()
	{
		static const bool value = !!path();
		return value;
	}

	struct image_stat
	{
		u64 count = 0;
		u64 bytes = 0;
	};

	struct spu_stat
	{
		u64 count = 0;
		u64 bytes = 0;
		u32 min_ea = umax;
		u32 max_ea = 0;
		u32 min_size = umax;
		u32 max_size = 0;
		u32 pc = 0;
	};

	struct state
	{
		std::mutex mutex;
		std::FILE* file = nullptr;
		std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
		u32 tables = 0;
		std::map<std::tuple<u32, u32, u32, u32, u32>, image_stat> readbacks; // address, width, height, pitch, 0
		std::map<std::tuple<u32, u32, u32, u32, u32>, image_stat> uploads;   // address, width, height, pitch, format
		std::map<std::tuple<u32, u32, u32>, spu_stat> transfers;             // program hash, kind, ea >> 16
		std::map<std::tuple<u32, u32, u32, u32, u32>, image_stat> blits;     // kind (0 scaled image, 1 buffer copy), destination, width, height, 0

		void flush_if_due()
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - last < std::chrono::seconds(5))
			{
				return;
			}

			if (!file)
			{
				file = std::fopen(path(), "w");
			}

			if (file)
			{
				std::fprintf(file, "table %u\n", tables++);
				for (const auto& [key, value] : readbacks)
				{
					std::fprintf(file, "readback address=0x%08x %ux%u pitch=%u count=%llu bytes=%llu\n", std::get<0>(key), std::get<1>(key), std::get<2>(key),
						std::get<3>(key), static_cast<unsigned long long>(value.count), static_cast<unsigned long long>(value.bytes));
				}
				for (const auto& [key, value] : uploads)
				{
					std::fprintf(file, "upload address=0x%08x %ux%u pitch=%u format=0x%x count=%llu\n", std::get<0>(key), std::get<1>(key), std::get<2>(key),
						std::get<3>(key), std::get<4>(key), static_cast<unsigned long long>(value.count));
				}
				for (const auto& [key, value] : blits)
				{
					std::fprintf(file, "%s destination=0x%08x %ux%u count=%llu\n", std::get<0>(key) ? "copy" : "blit", std::get<1>(key), std::get<2>(key),
						std::get<3>(key), static_cast<unsigned long long>(value.count));
				}
				for (const auto& [key, value] : transfers)
				{
					static constexpr const char* kinds[]{"PUT", "GET", "PUTLLUC", "PUTLLC"};
					std::fprintf(file, "spu program=%08x %s region=0x%04x0000 ea=0x%08x..0x%08x size=%u..%u count=%llu bytes=%llu pc=0x%x\n", std::get<0>(key),
						kinds[std::get<1>(key)], std::get<2>(key), value.min_ea, value.max_ea, value.min_size, value.max_size,
						static_cast<unsigned long long>(value.count), static_cast<unsigned long long>(value.bytes), value.pc);
				}
				std::fflush(file);
			}

			readbacks.clear();
			uploads.clear();
			transfers.clear();
			blits.clear();
			last = now;
		}
	};

	inline state& get()
	{
		static state s;
		return s;
	}

	inline void note_readback(u32 address, u32 bytes, u32 width, u32 height, u32 pitch)
	{
		auto& s = get();
		std::lock_guard lock(s.mutex);
		auto& stat = s.readbacks[{address, width, height, pitch, 0u}];
		stat.count++;
		stat.bytes += bytes;
		s.flush_if_due();
	}

	// A transfer of the blit engine: kind 0 = scaled image (NV3089), 1 = buffer copy (NV0039)
	inline void note_blit(u32 kind, u32 destination, u32 width, u32 height)
	{
		auto& s = get();
		std::lock_guard lock(s.mutex);
		s.blits[{kind, destination, width, height, 0u}].count++;
		s.flush_if_due();
	}

	// data is the guest memory at address. With RPCS3_ROUNDTRIP_SURVEY_DUMP=<directory> the 300th upload of each
	// texture is also written there, pitch * height bytes.
	inline void note_upload(u32 address, u32 width, u32 height, u32 pitch, u32 format, const void* data)
	{
		auto& s = get();
		std::lock_guard lock(s.mutex);
		s.uploads[{address, width, height, pitch, format}].count++;
		s.flush_if_due();

		static const char* dump_dir = std::getenv("RPCS3_ROUNDTRIP_SURVEY_DUMP");
		static std::map<std::tuple<u32, u32, u32>, u32> seen;
		if (dump_dir && ++seen[{address, width, height}] == 300)
		{
			char name[512];
			std::snprintf(name, sizeof(name), "%s/upload-%08x-%ux%u-pitch%u-format%02x.raw", dump_dir, address, width, height, pitch, format);
			if (std::FILE* out = std::fopen(name, "wb"))
			{
				std::fwrite(data, 1, usz{pitch} * height, out);
				std::fclose(out);
			}
		}
	}

	// kind: 0 PUT, 1 GET, 2 PUTLLUC, 3 PUTLLC. ls is the SPU local store: programs are told apart by a hash of the
	// code right after the usual job load address.
	inline void note_spu(const u8* ls, u32 kind, u32 ea, u32 size, u32 pc)
	{
		u32 hash = 0x811c9dc5;
		for (u32 i = 0x4040; i < 0x4140; i++)
		{
			hash = (hash ^ ls[i]) * 0x01000193;
		}

		auto& s = get();
		std::lock_guard lock(s.mutex);
		auto& stat = s.transfers[{hash, kind, ea >> 16}];
		stat.count++;
		stat.bytes += size;
		stat.min_ea = std::min<u32>(stat.min_ea, ea);
		stat.max_ea = std::max<u32>(stat.max_ea, ea + size);
		stat.min_size = std::min<u32>(stat.min_size, size);
		stat.max_size = std::max<u32>(stat.max_size, size);
		stat.pc = pc;
	}
}
