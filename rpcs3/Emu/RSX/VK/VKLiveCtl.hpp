#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "util/asm.hpp"

// Live-tunable experiment values, re-read about once a second from the file named by
// RPCS3_VK_LIVE_CTL (space separated integers). Index meanings:
//  0: periodic submit interval in us (0 = off)
//  1: minimum byte length for streaming (non-temporal) DMA loads (0 = off)
//  2: same-frame multi-block vertex cache (0/1)
//  3: source prefetch bitmask: 1 = vertex copies, 2 = blit complement upload, 4 = streaming DMA loads
//  4: minimum bytes for offloading vertex copies to the RSX offload thread (0 = off; needs RPCS3_RSX_COPY_OFFLOAD=1)
//  5: maximum spin multiplier for the SPU reservation-checking thread's MWAITX/TPAUSE wait (0 = stock 17/10)
//  6: PPU usleep addend override in microseconds, stored +1 (0 = use the config value)
//  7: SPU list GET transfers: bytes to prefetch per element of the following batch (0 = off)
//  9: repeat draws consumed straight from the FIFO (VKDraw.cpp, fast_draw_batch): 0 = off, 2 = on,
//     3 = off, but every draw that would qualify is checked: the complete path must leave all bound state as it was
//     4 = on, and after every texture change inside a run the complete path's program lookup must return the bound program
//     5 = on, but texture and polygon offset changes end a run (the earlier scope, for comparisons)
//     (1 = experiment: drop depth-only draws)
//  8: cross-frame cache of static vertex/index data (VKGeometryCache.hpp): 0 = off, 1 = on, 2 = on with content checks
// 10: compressed material image/sampler bindings: 0 = off, 1 = reuse, 2 = full path with every proposed reuse checked
// 11: clear validated unchanged pipeline state: 0 = stock, 1 = on, 2 = on with a fresh decode for every clean reuse
// 12: reuse of descriptor sets with identical contents (VKProgramPipeline.cpp, descriptor_table_t::commit): 0 = off, 1 = on
// 13: scale in percent of the inFamous particle target while the full resolution particles setting is on (0 = 250)
// 14: 1 = the inFamous particle depth buffer takes over its old contents again before it is filled (VKDraw.cpp, end())
namespace vk::live_ctl
{
	inline std::atomic<std::uint64_t> values[16]{};

	inline std::uint64_t get(unsigned index)
	{
		return values[index].load(std::memory_order_relaxed);
	}

	// Start cache-line transfers for a source range that another core most likely just wrote.
	inline void prefetch_range(const void* source, std::size_t length, std::size_t cap)
	{
		const auto* p = static_cast<const char*>(source);
		const std::size_t end = length < cap ? length : cap;
		for (std::size_t offset = 0; offset < end; offset += 64)
		{
			utils::prefetch_exec(p + offset);
		}
	}
}
