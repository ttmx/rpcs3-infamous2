#pragma once

// Diagnostic only: per-draw geometry sources (addresses, formats, content hashes) for a window of frames.
// RPCS3_VK_GEOM_TRACE=<output file>, armed when <output file>.arm appears, RPCS3_VK_GEOM_TRACE_FRAMES (default 6).
// RPCS3_VK_GEOM_TRACE_DUMP=<dir> additionally stores each distinct vertex/index byte range of the first traced frame.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace vk::geom_trace
{
	inline const char* path()
	{
		static const char* value = std::getenv("RPCS3_VK_GEOM_TRACE");
		return value && *value ? value : nullptr;
	}

	struct state_t
	{
		FILE* file = nullptr;
		unsigned long long first_frame = ~0ull;
		unsigned long long frames = 6;
		unsigned long long calls = 0;
		unsigned long long draw = 0;
		bool done = false;
		std::string dump_dir;
		std::unordered_set<unsigned long long> dumped;
	};

	inline state_t& state() { static state_t value; return value; }

	// RSX thread only. True while the frame window is open.
	inline bool active(unsigned long long frame)
	{
		if (!path()) return false;
		auto& s = state();
		if (s.done) return false;
		if (!s.file)
		{
			if ((s.calls++ & 1023) != 0) return false;
			std::error_code error;
			if (!std::filesystem::exists(std::string(path()) + ".arm", error) || error) return false;
			s.file = std::fopen(path(), "w");
			if (!s.file) { s.done = true; return false; }
			if (const char* n = std::getenv("RPCS3_VK_GEOM_TRACE_FRAMES")) s.frames = std::strtoull(n, nullptr, 10);
			if (const char* d = std::getenv("RPCS3_VK_GEOM_TRACE_DUMP")) s.dump_dir = d;
			// Start on the next frame boundary so the first traced frame is complete
			s.first_frame = frame + 1;
		}
		if (frame < s.first_frame) return false;
		if (frame >= s.first_frame + s.frames)
		{
			std::fclose(s.file);
			s.file = nullptr;
			s.done = true;
			return false;
		}
		return true;
	}

	inline unsigned long long hash(const void* data, std::size_t length)
	{
		const auto* p = static_cast<const unsigned char*>(data);
		unsigned long long h = 0x9e3779b97f4a7c15ull ^ (length * 0xff51afd7ed558ccdull);
		while (length >= 8)
		{
			unsigned long long v;
			std::memcpy(&v, p, 8);
			h = (h ^ v) * 0x9fb21c651e98df25ull;
			h ^= h >> 32;
			p += 8;
			length -= 8;
		}
		unsigned long long tail = 0;
		std::memcpy(&tail, p, length);
		h = (h ^ tail) * 0x9fb21c651e98df25ull;
		return h ^ (h >> 29);
	}

	// Store one distinct byte range of the first traced frame
	inline void dump(char kind, unsigned long long frame, unsigned address, const void* data, std::size_t length, unsigned long long h)
	{
		auto& s = state();
		if (s.dump_dir.empty() || frame != s.first_frame || !s.dumped.insert(h ^ address).second) return;
		char name[64];
		std::snprintf(name, sizeof(name), "/%c-%08x-%zx-%016llx.bin", kind, address, length, h);
		if (FILE* f = std::fopen((s.dump_dir + name).c_str(), "wb"))
		{
			std::fwrite(data, 1, length, f);
			std::fclose(f);
		}
	}
}
