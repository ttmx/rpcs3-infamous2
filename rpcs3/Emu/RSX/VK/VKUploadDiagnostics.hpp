#pragma once

// Diagnostic only: exact upload requests, never a content-validity cache.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>

namespace vk::upload_diagnostics
{
	inline bool enabled()
	{
		static const bool value = []
		{
			const char* flag = std::getenv("RPCS3_VK_UPLOAD_TRACE");
			const char* path = std::getenv("RPCS3_VK_UPLOAD_TRACE_PATH");
			return flag && std::strcmp(flag, "1") == 0 && path && *path;
		}();
		return value;
	}

	inline unsigned long long now_ns()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	class output
	{
		std::mutex mutex;
		FILE* file = nullptr;
		unsigned long long rows = 0;
		std::atomic<unsigned long long> dropped{0};
		std::atomic<bool> exhausted{false};
		static constexpr unsigned long long row_cap = 500000;

	public:
		output()
		{
			file = std::fopen(std::getenv("RPCS3_VK_UPLOAD_TRACE_PATH"), "w");
			if (file)
			{
				std::fputs("kind,mono_ns,frame,draw,ordinal,address,length,stride,location,attribute_mask,attributes,heap_uid,memory_type,memory_flags,persistent_bytes,volatile_bytes,persistent_allocated,persistent_copy_requested,volatile_copy_requested,cache_eligible,cache_hit,draw_command,strict,multithreaded,dropped_batches\n", file);
				std::fprintf(file, "start,%llu,0,0,0,0,0,0,0,0,,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", now_ns());
				std::fflush(file);
			}
			else exhausted.store(true, std::memory_order_relaxed);
		}

		~output()
		{
			if (file)
			{
				std::fprintf(file, "stop,%llu,0,0,0,0,0,0,0,0,,0,0,0,0,0,0,0,0,0,0,0,0,0,%llu\n", now_ns(), dropped.load(std::memory_order_relaxed));
				std::fclose(file);
			}
		}

		bool active()
		{
			if (!exhausted.load(std::memory_order_relaxed)) return true;
			dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		void write(const std::string& batch, unsigned long long count)
		{
			std::lock_guard lock(mutex);
			if (exhausted.load(std::memory_order_relaxed))
			{
				dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (rows + count > row_cap)
			{
				dropped.fetch_add(1, std::memory_order_relaxed);
				std::fprintf(file, "cap,%llu,0,0,0,0,0,0,0,0,,0,0,0,0,0,0,0,0,0,0,0,0,0,%llu\n", now_ns(), dropped.load(std::memory_order_relaxed));
				std::fflush(file);
				exhausted.store(true, std::memory_order_relaxed);
				return;
			}
			std::fwrite(batch.data(), 1, batch.size(), file);
			rows += count;
			if ((rows / 2048) != ((rows - count) / 2048)) std::fflush(file);
		}
	};

	inline output& writer() { static output value; return value; }
	inline bool active()
	{
		if (!enabled()) return false;
		static const std::string arm_path = []
		{
			const char* path = std::getenv("RPCS3_VK_UPLOAD_TRACE_ARM_FILE");
			return path ? std::string(path) : std::string{};
		}();
		if (!arm_path.empty())
		{
			static std::atomic<unsigned> armed{0}; // 0 waiting, 1 emitting status, 2 armed.
			static std::atomic<unsigned long long> calls{0};
			if (armed.load(std::memory_order_acquire) != 2)
			{
				if ((calls.fetch_add(1, std::memory_order_relaxed) & 255) != 0) return false;
				std::error_code error;
				if (!std::filesystem::exists(arm_path, error) || error) return false;
				unsigned expected = 0;
				if (armed.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
				{
					auto& out = writer();
					out.write("armed," + std::to_string(now_ns()) + ",0,0,0,0,0,0,0,0,,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 1);
					armed.store(2, std::memory_order_release);
				}
				if (armed.load(std::memory_order_acquire) != 2) return false;
			}
		}
		return writer().active();
	}
}
