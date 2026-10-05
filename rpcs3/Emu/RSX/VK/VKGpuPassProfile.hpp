#pragma once

// Diagnostic only (RPCS3_VK_GPU_PASS_PROFILE=1): GPU time per render target pair and per native pass. Every 30th
// frame gets a GPU timestamp wherever the work changes (another render target pair, a blit, one of the GPU passes of
// VKNativeSSAO.cpp and VKNativeLighting.cpp); the time between two timestamps goes to the earlier one's label.
// Reported every 20 sampled frames. The time includes any wait of the GPU for the next submission, so it is only
// meaningful while the GPU is the limit. Presentation is not included.
#include "VKHelpers.h"
#include "VKRenderPass.h"
#include "vkutils/device.h"
#include "vkutils/commands.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace vk::gpu_pass_profile
{
	inline bool enabled()
	{
		static const bool value = [] { const char* flag = std::getenv("RPCS3_VK_GPU_PASS_PROFILE"); return flag && std::strcmp(flag, "1") == 0; }();
		return value;
	}

	// Labels that are not a render target pair (colour address in the high half, depth address in the low half)
	enum : u64
	{
		label_blit = 1, label_normals_blit, label_depth_blit, label_occlusion, label_lighting_latch, label_lighting_setup,
		label_lighting_pass, // and the four after it
		label_lighting_user = label_lighting_pass + 5, label_occlusion_user,
		label_end
	};

	struct total_t
	{
		u64 ticks = 0, segments = 0, draws = 0;
		u32 width = 0, height = 0;
	};

	struct state_t
	{
		static constexpr u32 capacity = 2048;
		VkQueryPool pool = VK_NULL_HANDLE;
		std::vector<u64> labels;      // Of the timestamps written in the sampled frame
		std::map<u64, total_t> totals;
		u64 last_label = 0, frames = 0, sampled = 0, pending_since = 0;
		bool sampling = false, pending = false;
	};

	inline state_t& state() { static state_t value; return value; }

	inline void stamp(VkCommandBuffer cmd, state_t& s, u64 label)
	{
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, ::size32(s.labels));
		s.labels.push_back(label);
		s.last_label = label;
	}

	// Work with this label starts here
	inline void mark(VkCommandBuffer cmd, u64 label, u32 width = 0, u32 height = 0)
	{
		auto& s = state();
		if (!s.sampling) return;

		auto& total = s.totals[label];
		total.draws++;
		total.width = width;
		total.height = height;

		if (label != s.last_label && s.labels.size() + 1 < state_t::capacity)
		{
			stamp(cmd, s, label);
		}
	}

	inline void report(state_t& s)
	{
		const auto& device = *vk::get_current_renderer();
		const double ms_per_tick = device.gpu().get_limits().timestampPeriod / 1e6 / s.sampled;
		std::vector<std::pair<u64, total_t>> rows(s.totals.begin(), s.totals.end());
		std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });

		std::string out;
		double sum = 0;

		for (const auto& [label, total] : rows)
		{
			static const char* const names[] = { "", "blit", "G-buffer blit, normals", "G-buffer blit, depth", "occlusion", "lighting latch", "lighting setup and clears", "lighting: lights", "lighting: bounds", "lighting: cull", "lighting: shade", "lighting: clear", "rest of the draw that binds the lighting", "after occlusion, to the next draw" };
			const double ms = total.ticks * ms_per_tick;
			sum += ms;
			if (ms < 0.02) continue;
			out += label < label_end ? fmt::format(" [%s: %.2f ms]", names[label], ms) :
				fmt::format(" [%08x/%08x %ux%u: %.2f ms, %.0f draws, %.1f segments]", static_cast<u32>(label >> 32), static_cast<u32>(label), total.width, total.height,
					ms, 1. * total.draws / s.sampled, 1. * total.segments / s.sampled);
		}

		rsx_log.notice("GPU pass profile per frame, total %.2f ms:%s", sum, out);
		s.totals.clear();
		s.sampled = 0;
	}

	// Start of a flip: ends the sampled frame, collects an earlier one, or starts sampling the next
	inline void frame(vk::command_buffer& cmd)
	{
		if (!enabled()) return;

		auto& s = state();
		const auto& device = *vk::get_current_renderer();
		s.frames++;

		if (!s.pool)
		{
			VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			info.queryType = VK_QUERY_TYPE_TIMESTAMP;
			info.queryCount = state_t::capacity;
			if (vkCreateQueryPool(device, &info, nullptr, &s.pool) != VK_SUCCESS) return;
		}

		if (s.sampling)
		{
			stamp(cmd, s, label_end);
			s.sampling = false;
			s.pending = true;
			s.pending_since = s.frames;
		}
		else if (s.pending)
		{
			if (s.frames - s.pending_since < 10) return;

			std::vector<u64> ticks(s.labels.size());
			if (vkGetQueryPoolResults(device, s.pool, 0, ::size32(ticks), ticks.size() * sizeof(u64), ticks.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;

			for (usz i = 0; i + 1 < ticks.size(); i++)
			{
				auto& total = s.totals[s.labels[i]];
				total.ticks += ticks[i + 1] - ticks[i];
				total.segments++;
			}

			s.pending = false;
			if (++s.sampled == 20) report(s);
		}
		else if (s.frames % 30 == 0)
		{
			if (vk::is_renderpass_open(cmd)) vk::end_renderpass(cmd);
			vkCmdResetQueryPool(cmd, s.pool, 0, state_t::capacity);
			s.labels.clear();
			s.last_label = 0;
			s.sampling = true;
		}
	}
}
